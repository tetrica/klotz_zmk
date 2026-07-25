#include <stdint.h>
#define DT_DRV_COMPAT azoteq_tps43

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <errno.h>

#include "tps43.h"

LOG_MODULE_REGISTER(tps43, CONFIG_INPUT_LOG_LEVEL);

/* Uptime (ms) of the last cursor MOVEMENT. The display code (src/custom_status_
 * screen.c) reads this via extern to blank the OLED while the trackpad is in use:
 * the OLED and trackpad share the 3.3V rail, so the OLED's draw dips the rail and
 * the capacitive sensor reads the dip as noise -> cursor jumps. Only updated on
 * the half that actually has the trackpad. 0 = no activity yet. */
volatile int64_t tps43_last_activity_ms = 0;
 
/**
 * @brief Завершает окно связи с тачпадом
 * 
 * После каждого чтения регистров тачпада необходимо завершить окно связи,
 * записав специальный адрес 0xEEEE, что вызывает NACK от устройства.
 * Это обязательный шаг согласно протоколу IQS5xx.
 * 
 * @param dev Указатель на устройство тачпада
 */
static void tps43_end_communication_window(const struct device *dev) {
    const struct tps43_config *config = dev->config;
    uint8_t end_buf[2];

    sys_put_be16(TPS43_REG_END_COMM_WINDOW, end_buf);

    int ret = i2c_write_dt(&config->i2c_bus, end_buf, sizeof(end_buf));
    if (ret != 0 && ret != -EIO) {
        LOG_INF("Запись окончания окна связи вернула: %d (ожидается NACK)", ret);
    }
}

/**
 * @brief Читает последовательность регистров тачпада
 * 
 * Выполняет чтение нескольких байт из последовательных регистров тачпада,
 * начиная с указанного адреса. Используется для чтения связанных регистров,
 * таких как события жестов (GESTURE_EVENTS_0 и GESTURE_EVENTS_1).
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес начального регистра (16-битный)
 * @param val Указатель на буфер для данных
 * @param len Количество байт для чтения
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int read_sequence_registers(const struct device *dev, uint16_t reg, void *val, size_t len) {
    const struct tps43_config *config = dev->config;
    uint8_t addr_buf[2];
    addr_buf[0] = (uint8_t)((reg >> 8) & 0xFF);
    addr_buf[1] = (uint8_t)(reg & 0xFF);

    return i2c_write_read_dt(&config->i2c_bus, addr_buf, 2, val, len);
}

/**
 * @brief Читает 16-битный регистр тачпада через I2C
 * 
 * Выполняет чтение 16-битного значения из указанного регистра тачпада.
 * Данные интерпретируются как big-endian (MSB first).
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес регистра (16-битный)
 * @param val Указатель на переменную для сохранения прочитанного значения
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_i2c_read_reg16(const struct device *dev, uint16_t reg, uint16_t *val)
{
    const struct tps43_config *config = dev->config;
    uint8_t buf[2];
    // формирует 2-байтовый адрес регистра: (MSB, LSB)
    // MSB: сдвиг на 8 бит вправо (0x2F00 -> 0x2F)
    // LSB: побитовое И с маской - маска, оставляет только младший байт (0x2F00 -> 0x00)
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};
    int ret;
    
    // записывает адрес регистра (reg_buf) и читает 2 байта данных (в буфер buf)
    ret = i2c_write_read_dt(&config->i2c_bus, reg_buf, sizeof(reg_buf), buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Ошибка чтения регистра 0x%04x: %d", reg, ret);
        return ret;
    }
    
    // преобразует big-endian данные (MSB first) обратно в 16-битное значение
    *val = (buf[0] << 8) | buf[1];
    return 0;
}

/**
 * @brief Записывает 16-битное значение в регистр тачпада через I2C
 * 
 * Выполняет запись 16-битного значения в указанный регистр тачпада.
 * Данные передаются как big-endian (MSB first).
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес регистра (16-битный)
 * @param val Значение для записи (16-битное)
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_i2c_write_reg16(const struct device *dev, uint16_t reg, uint16_t val)
{
    const struct tps43_config *config = dev->config;
    // формирует 4-байтовый адрес регистра: (MSB, LSB, MSB_VALUE, LSB_VALUE)
    uint8_t buf[4] = {reg >> 8, reg & 0xFF, val >> 8, val & 0xFF};
    int ret;
    
    ret = i2c_write_dt(&config->i2c_bus, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Ошибка записи регистра 0x%04x: %d", reg, ret);
        return ret;
    }
    
    return 0;
}

/**
 * @brief Читает 8-битный регистр тачпада через I2C
 * 
 * Выполняет чтение 8-битного значения из указанного регистра тачпада.
 * Используется для чтения большинства регистров конфигурации и статуса.
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес регистра (16-битный)
 * @param val Указатель на переменную для сохранения прочитанного значения
 * @param with_err Признак логирования ошибки или ожидаемое поведение
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_i2c_read_reg8_w_err(const struct device *dev, uint16_t reg, uint8_t *val, bool with_err)
{
    const struct tps43_config *config = dev->config;
    // формирует 2-байтовый адрес регистра: (MSB, LSB)
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};
    int ret;
    
    ret = i2c_write_read_dt(&config->i2c_bus, reg_buf, sizeof(reg_buf), val, 1);
    if (ret != 0) {
        if (!with_err) {
            LOG_INF("Ожидаемое завершение чтение регистра 0x%04x: %d", reg, ret);
        } else {
            LOG_ERR("Ошибка чтения регистра 0x%04x: %d", reg, ret);
        }
        return ret;
    }
    return 0;
}

static inline int tps43_i2c_read_reg8(const struct device *dev, uint16_t reg, uint8_t *val)
{
    return tps43_i2c_read_reg8_w_err(dev, reg, val, true);
}

/**
 * @brief Записывает 8-битное значение в регистр тачпада через I2C
 * 
 * Выполняет запись 8-битного значения в указанный регистр тачпада.
 * Используется для записи регистров конфигурации и управления.
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес регистра (16-битный)
 * @param val Значение для записи (8-битное)
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_i2c_write_reg8(const struct device *dev, uint16_t reg, uint8_t val)
{
    const struct tps43_config *config = dev->config;
    uint8_t buf[3] = {reg >> 8, reg & 0xFF, val};
    int ret;
    
    ret = i2c_write_dt(&config->i2c_bus, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Ошибка записи регистра 0x%04x: %d", reg, ret);
        return ret;
    }
    
    return 0;
}

/**
 * @brief Callback обработчик прерывания от пина RDY тачпада
 * 
 * Вызывается при изменении состояния пина RDY (Ready) тачпада,
 * который сигнализирует о наличии новых данных для чтения.
 * Планирует выполнение обработчика работы для чтения данных.
 * 
 * @param dev Указатель на устройство тачпада
 * @param cb Указатель на структуру callback GPIO
 * @param pins Маска пинов, вызвавших прерывание
 */
 static void tps43_rdy_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
     struct tps43_drv_data *drv_data = CONTAINER_OF(cb, struct tps43_drv_data, rdy_cb);
 
     k_work_submit(&drv_data->work);
 }
 
/**
 * @brief Внутренняя функция для перевода тачпада в режим suspend/resume
 * 
 * Управляет регистром SYSTEM_CONTROL_1 (0x0432), устанавливая или снимая бит SUSPEND.
 * В режиме suspend тачпад переходит в состояние низкого энергопотребления и не обрабатывает
 * касания до пробуждения.
 * 
 * @param dev Указатель на устройство тачпада
 * @param suspend true - перевести в suspend, false - вывести из suspend
 * @param lock_held true если семафор уже захвачен (для внутреннего использования)
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_set_suspend_internal(const struct device *dev, bool suspend, bool lock_held) {
    struct tps43_drv_data *drv_data = dev->data;
    const struct tps43_config *config = dev->config;
    int ret = 0;

    // Если управление питанием отключено, ничего не делаем
    if (!config->enable_power_management) {
        return 0;
    }

    // Захватываем семафор, если он еще не захвачен
    if (!lock_held) {
        if (k_sem_take(&drv_data->lock, K_MSEC(100)) != 0) {
            LOG_WRN("Не удалось захватить семафор для suspend/resume");
            return -EBUSY;
        }
    }

    // Отключаем прерывания RDY при переходе в suspend (до любых I2C операций)
    // Это предотвращает race condition когда RDY срабатывает между попыткой suspend и установкой признака
    if (suspend && config->rdy_gpio.port != NULL) {
        ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_DISABLE);
        if (ret == 0) {
            LOG_INF("Прерывания RDY отключены перед suspend");
        }
    }

    uint8_t control_reg = 0;
    
    // При выходе из suspend первая транзакция вернет NACK (п.7.3.1)
    if (drv_data->suspended && !suspend) {
        ret = tps43_i2c_read_reg8_w_err(dev, TPS43_REG_SYSTEM_CONTROL_1, &control_reg, false);
        k_sleep(K_MSEC(200));
        LOG_INF("I2C Wake: устройство пробуждено из suspend");
        
        // После пробуждения читаем регистр повторно
        ret = tps43_i2c_read_reg8_w_err(dev, TPS43_REG_SYSTEM_CONTROL_1, &control_reg, false);

    } else if (!drv_data->suspended) {
        // Читаем текущее значение только если не в suspend
        ret = tps43_i2c_read_reg8_w_err(dev, TPS43_REG_SYSTEM_CONTROL_1, &control_reg, false);
        if (ret != 0) {
            // Если ошибка -5 (EIO) при попытке перевести в suspend - устройство уже в suspend
            if (ret == -EIO && suspend) {
                LOG_INF("Устройство уже в suspend (ошибка I2C)");
                drv_data->suspended = true;
                ret = 0;
                goto done;
            }
            LOG_ERR("Ошибка чтения SYSTEM_CONTROL_1: %d", ret);
            goto done;
        }
    }

    if (suspend) {
        control_reg |= TPS43_SUSPEND;
        LOG_INF("Перевод в suspend (низкое энергопотребление)");
    } else {
        control_reg &= ~TPS43_SUSPEND;
        LOG_INF("Выход из suspend");
    }

    ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONTROL_1, control_reg);
    if (ret != 0) {
        if (ret == -EIO && suspend) {
            LOG_INF("Не удалось записать suspend, устройство уже в suspend");
            drv_data->suspended = true;
            ret = 0;
            goto done;
        }
        LOG_ERR("Ошибка записи SYSTEM_CONTROL_1: %d", ret);
        goto done;
    }

    drv_data->suspended = suspend;

done:
    // Включаем прерывания RDY после resume
    if (!suspend && config->rdy_gpio.port != NULL) {
        ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_EDGE_TO_ACTIVE);
        if (ret == 0) {
            LOG_INF("Прерывания RDY включены");
        }
    }
    tps43_end_communication_window(dev);
    if (!lock_held) {
        k_sem_give(&drv_data->lock);
    }
    return ret;
}

/**
 * @brief Основной обработчик работы для обработки событий тачпада
 * 
 * Выполняется при получении прерывания от тачпада (RDY pin).
 * Читает и обрабатывает события жестов, движения курсора и прокрутки,
 * преобразуя их в события ввода для системы ZMK.
 * Также управляет пробуждением тачпада из режима suspend при обнаружении активности.
 * 
 * Защищен семафором для предотвращения прерывания другими операциями I2C,
 * что обеспечивает плавное движение курсора без прерываний.
 * 
 * @param work Указатель на структуру работы
 */
static void tps43_work_handler(struct k_work *work) {
    struct tps43_drv_data *drv_data = CONTAINER_OF(work, struct tps43_drv_data, work);
    const struct device *dev = drv_data->dev;
    const struct tps43_config *config = dev->config;
    bool is_scroll_active = drv_data->scroll_active;
    bool is_drag_active = drv_data->drag_active;
    bool is_three_drag = drv_data->three_drag_active;
    int ret;
    
    // Если устройство в suspend, игнорируем прерывание (RDY должен быть отключен)
    if (drv_data->suspended) {
        LOG_WRN("Прерывание RDY в suspend режиме - игнорируем");
        return;
    }
    
    // Захватываем семафор для защиты всех операций I2C от прерывания
    // Это предотвращает конфликты при одновременном доступе к тачпаду
    k_sem_take(&drv_data->lock, K_FOREVER);

    uint8_t sys_info = 0;
    ret = tps43_i2c_read_reg8(dev, TPS43_REG_SYSTEM_INFO_1, &sys_info);
    if (ret < 0) {
        LOG_ERR("Ошибка чтения системной информации: %d", ret);
        goto done;
    }

    uint8_t gestures_events[2];
    ret = read_sequence_registers(dev, TPS43_REG_GESTURE_EVENTS_0, &gestures_events, 2);
    if (ret < 0) {
        LOG_ERR("Ошибка чтения событий жестов: %d", ret);
        goto done;
    }

    if (gestures_events[0] != 0 || gestures_events[1] != 0) {

        LOG_INF("Жесты: Одиночное=0x%02X, Мульти=0x%02X", gestures_events[0], gestures_events[1]);

        if (gestures_events[0] & TPS43_SINGLE_TAP) {
            LOG_INF("Одиночное касание → ЛЕВАЯ КНОПКА");
            input_report_key(dev, INPUT_BTN_0, 1, true, K_FOREVER);
            input_report_key(dev, INPUT_BTN_0, 0, true, K_FOREVER);
            /* Arm tap-then-hold drag: a fresh touch landing within the window
             * after this tap will grab and drag (see tap-drag block below). */
            if (config->tap_drag) {
                drv_data->last_tap_time = k_uptime_get();
            }
        }
        if (gestures_events[1] & TPS43_TWO_FINGER_TAP) {
            LOG_INF("Касание двумя пальцами → ПРАВАЯ КНОПКА");
            input_report_key(dev, INPUT_BTN_1, 1, true, K_FOREVER);  
            input_report_key(dev, INPUT_BTN_1, 0, true, K_FOREVER); 
        }
        /* Native chip press-and-hold drag (only when explicitly enabled; tap-drag
         * manages is_drag_active on its own, so keep these gated to avoid a
         * spurious release mid tap-drag). */
        if (config->press_and_hold && (gestures_events[0] & TPS43_PRESS_AND_HOLD) && (!(is_drag_active))) {
            LOG_INF("Обнаружено нажатие и удержание - ПЕРЕТАСКИВАНИЕ (УДЕРЖАНИЕ ЛЕВОЙ КНОПКИ)");
            // установить внутренний флаг на перетаскивание и нажимает левую кнопку мыши
            is_drag_active = true;
            input_report_key(dev, INPUT_BTN_0, 1, true, K_FOREVER); 
        }
        if (config->press_and_hold && (!(gestures_events[0] & TPS43_PRESS_AND_HOLD)) && (is_drag_active)) {
            LOG_INF("Обнаружено окончание нажатия и удержания - ОТПУСКАНИЕ (ОТПУСК ЛЕВОЙ КНОПКИ)");
            // установить внутренний флаг на перетаскивание и нажимает левую кнопку мыши
            is_drag_active = false;
            input_report_key(dev, INPUT_BTN_0, 0, true, K_FOREVER);   // отпускание + синхронизация
        }
        if (gestures_events[1] & TPS43_SCROLL) {
            LOG_INF("Обнаружена прокрутка - Прокрутка");
            // устанавливаем признак скролла для обработки в блоке tp_movement
            is_scroll_active = true;
        }
    }

    /* Robust release for the press-and-hold LEFT drag. The chip usually emits NO
     * gesture event on the finger-lift cycle, so the in-gesture release above
     * gets skipped and BTN_0 stays stuck DOWN -> the next single tap then just
     * releases the stuck button instead of clicking (state desync). Release here
     * on finger lift (count == 0) as the source of truth. */
    if (config->press_and_hold && is_drag_active) {
        uint8_t nf = 0;
        (void)tps43_i2c_read_reg8(dev, TPS43_REG_NUM_FINGERS, &nf);
        if (nf == 0) {
            LOG_INF("Press-hold: отпускание левой кнопки по отрыву пальца");
            is_drag_active = false;
            input_report_key(dev, INPUT_BTN_0, 0, true, K_FOREVER);
        }
    }

    /* Tap-then-hold drag ("double-tap-drag"): a fresh touch (finger count going
     * 0 -> >=1) that lands within tap_drag_window_ms of a single tap grabs by
     * holding the left button; releasing all fingers releases it. This runs
     * OUTSIDE the movement block so the release is detected even when the final
     * lift carries no movement. */
    if (config->tap_drag) {
        uint8_t num_fingers_now = 0;
        (void)tps43_i2c_read_reg8(dev, TPS43_REG_NUM_FINGERS, &num_fingers_now);

        if (!is_drag_active && drv_data->prev_num_fingers == 0 && num_fingers_now >= 1 &&
            drv_data->last_tap_time != 0 &&
            (k_uptime_get() - drv_data->last_tap_time) <= config->tap_drag_window_ms) {
            LOG_INF("Tap-drag: захват (удержание левой кнопки)");
            is_drag_active = true;
            drv_data->last_tap_time = 0;
            input_report_key(dev, INPUT_BTN_0, 1, true, K_FOREVER);
        }
        if (is_drag_active && num_fingers_now == 0) {
            LOG_INF("Tap-drag: отпускание левой кнопки");
            is_drag_active = false;
            input_report_key(dev, INPUT_BTN_0, 0, true, K_FOREVER);
        }
        drv_data->prev_num_fingers = num_fingers_now;
    }

    /* Three-finger middle-button drag: hold the MIDDLE button while >=3 fingers
     * touch, release when the count drops below 3. A quick 3-finger tap becomes
     * a middle click; a 3-finger hold + move becomes a middle-drag (the cursor
     * movement below is emitted normally while the button is held) -> lets you
     * rotate/pan a 3D view in a slicer. Runs OUTSIDE the movement block so the
     * press/release fire even with no movement. */
    if (config->three_finger_drag) {
        uint8_t nf = 0;
        (void)tps43_i2c_read_reg8(dev, TPS43_REG_NUM_FINGERS, &nf);
        if (!is_three_drag && nf >= 3) {
            LOG_INF("3-finger drag: захват (удержание средней кнопки)");
            is_three_drag = true;
            input_report_key(dev, INPUT_BTN_2, 1, true, K_FOREVER);
        }
        if (is_three_drag && nf < 3) {
            LOG_INF("3-finger drag: отпускание средней кнопки");
            is_three_drag = false;
            input_report_key(dev, INPUT_BTN_2, 0, true, K_FOREVER);
        }
    }

    if (sys_info & TPS43_TP_MOVEMENT) {
        int16_t rel_x = 0, rel_y = 0;
        ret = tps43_i2c_read_reg16(dev, TPS43_REG_REL_X, (uint16_t*)&rel_x);
        if (ret < 0) {
            LOG_ERR("Ошибка чтения REL_X: %d", ret);
            goto done;
        }
        ret = tps43_i2c_read_reg16(dev, TPS43_REG_REL_Y, (uint16_t*)&rel_y);
        if (ret < 0) {
            LOG_ERR("Ошибка чтения REL_Y: %d", ret);
            goto done;
        }
        // Отправляем движение курсора
        if (rel_x != 0 || rel_y != 0) {
            /* Mark active use so the display code blanks the shared-rail OLED. */
            tps43_last_activity_ms = k_uptime_get();
            if (rel_x != 0 ) {
                int32_t scaled_x = ((int32_t)rel_x * config->sensitivity) / 100;
                rel_x = (int16_t)CLAMP(scaled_x, INT16_MIN, INT16_MAX);
            }
            if (rel_y != 0) { 
                int32_t scaled_y = ((int32_t)rel_y * config->sensitivity) / 100;
                rel_y = (int16_t)CLAMP(scaled_y, INT16_MIN, INT16_MAX);
            }
            LOG_INF("Отправка движения: dx=%d, dy=%d", rel_x, rel_y);

            // Обработка свайпов тремя пальцами
            if (config->swipes) {
                uint8_t num_fingers = 0;
                ret = tps43_i2c_read_reg8(dev, TPS43_REG_NUM_FINGERS, &num_fingers);
                if (ret < 0) {
                    LOG_ERR("Ошибка чтения NUM_FINGERS: %d", ret);
                    goto done;
                }
                if (num_fingers == 3) {
                    if (rel_x < 0) {
                        LOG_INF("Свайп 3 пальцами влево - кнопка 6 мыши");
                        input_report_key(dev, INPUT_BTN_6, 1, true, K_FOREVER);
                        input_report_key(dev, INPUT_BTN_6, 0, true, K_FOREVER);
                    }
                    if (rel_x > 0) {
                        LOG_INF("Свайп 3 пальцами вправо - кнопка 7 мыши");
                        input_report_key(dev, INPUT_BTN_7, 1, true, K_FOREVER);
                        input_report_key(dev, INPUT_BTN_7, 0, true, K_FOREVER);
                    }
                }
            }

        
            if (is_scroll_active) {
                /* Scroll ONLY while two fingers are physically down. The IQS5xx keeps
                 * reporting the SCROLL gesture for the whole touch session (until ALL
                 * fingers lift), so lifting one finger to keep navigating would stay
                 * stuck in scroll mode. Re-check the live finger count each event and
                 * fall back to cursor movement the moment it drops below two. */
                uint8_t num_fingers = 0;
                (void)tps43_i2c_read_reg8(dev, TPS43_REG_NUM_FINGERS, &num_fingers);
                if (num_fingers >= 2) {
                    /* Smooth scroll via a fractional accumulator: keep the
                     * remainder between events instead of truncating each time,
                     * so low sensitivities emit an even 1-line cadence instead of
                     * 0,0,0,N bursts (the source of the jerkiness). Only the
                     * dominant axis scrolls; the other axis' accumulator resets. */
                    if (abs(rel_x) > abs(rel_y)) {
                        // Горизонтальный скролл
                        if (config->invert_scroll_x) {
                            rel_x = -rel_x;
                        }
                        drv_data->scroll_accum_x += (int32_t)rel_x * config->scroll_sensitivity;
                        int16_t wheel = (int16_t)(drv_data->scroll_accum_x / 100);
                        if (wheel != 0) {
                            drv_data->scroll_accum_x -= (int32_t)wheel * 100;
                            input_report_rel(dev, INPUT_REL_HWHEEL, wheel, true, K_FOREVER);
                        }
                        drv_data->scroll_accum_y = 0;
                    } else {
                        // Вертикальный скролл
                        if (config->invert_scroll_y) {
                            rel_y = -rel_y;
                        }
                        drv_data->scroll_accum_y += (int32_t)rel_y * config->scroll_sensitivity;
                        int16_t wheel = (int16_t)(drv_data->scroll_accum_y / 100);
                        if (wheel != 0) {
                            drv_data->scroll_accum_y -= (int32_t)wheel * 100;
                            input_report_rel(dev, INPUT_REL_WHEEL, wheel, true, K_FOREVER);
                        }
                        drv_data->scroll_accum_x = 0;
                    }
                } else {
                    // Меньше двух пальцев -> завершаем скролл, двигаем курсор
                    drv_data->scroll_accum_x = 0;
                    drv_data->scroll_accum_y = 0;
                    input_report_rel(dev, INPUT_REL_X, rel_x, false, K_FOREVER);
                    input_report_rel(dev, INPUT_REL_Y, rel_y, true, K_FOREVER);
                }
                is_scroll_active = false;
            } else {
                // Обычное движение курсора (сбрасываем накопители скролла)
                drv_data->scroll_accum_x = 0;
                drv_data->scroll_accum_y = 0;
                input_report_rel(dev, INPUT_REL_X, rel_x, false, K_FOREVER);
                input_report_rel(dev, INPUT_REL_Y, rel_y, true, K_FOREVER);
            }
        }
    }

done:
    // Сохраняем для следующего вызова
    drv_data->scroll_active = is_scroll_active;
    drv_data->drag_active = is_drag_active;
    drv_data->three_drag_active = is_three_drag;
    tps43_end_communication_window(dev);
    
    // Освобождаем семафор после завершения всех операций I2C
    k_sem_give(&drv_data->lock);
}

/**
 * @brief Сбрасывает внутренние значения состояния драйвера
 * 
 * Инициализирует все флаги состояния драйвера в начальные значения.
 * Используется при инициализации и сбросе устройства.
 * 
 * @param dev Указатель на устройство тачпада
 * @return 0 при успехе
 */
static int tps43_reset_values(const struct device *dev) {
    struct tps43_drv_data *drv_data = dev->data;

    drv_data->device_ready = false;
    drv_data->initialized = false;
    drv_data->scroll_active = false;
    drv_data->drag_active = false;
    drv_data->last_tap_time = 0;
    drv_data->prev_num_fingers = 0;
    drv_data->three_drag_active = false;
    drv_data->scroll_accum_x = 0;
    drv_data->scroll_accum_y = 0;

    LOG_INF("Сброс значений");
    return 0;
}

/**
 * @brief Конфигурирует системные регистры тачпада для работы
 * 
 * Настраивает регистры тачпада для отслеживания событий касания, жестов и движения.
 * Включает необходимые жесты (single tap, press and hold, scroll, two finger tap),
 * настраивает инверсию осей и устанавливает флаг завершения настройки.
 * 
 * @param dev Указатель на устройство тачпада
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_configure_device(const struct device *dev) {

    const struct tps43_config *config = dev->config;
    int ret;

    // запись в TPS43_REG_SYSTEM_CONFIG_1 событий для отслеживания  
    uint8_t events_to_track = TPS43_TP_EVENT | TPS43_EVENT_MODE;
    
    // Жесты (single_tap, press_and_hold, scroll, two_finger_tap)
    if (config->single_tap || config->press_and_hold || 
        config->scroll || config->two_finger_tap) {
        events_to_track |= TPS43_GESTURE_EVENT;
    }
    
    // Touch events для абсолютных координат
    events_to_track |= TPS43_TOUCH_EVENT;
    
    ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONFIG_1, events_to_track);
    if (ret != 0) {
        LOG_WRN("Ошибка записи событий для отслеживания: %d", ret);
        return ret;
    }
    LOG_INF("События сконфигурированы: 0x%02X", events_to_track);

    // конфигурация осей
    uint8_t xy_config = 0;
    xy_config |= config->invert_x ? TPS43_FLIP_X : 0;
    xy_config |= config->invert_y ? TPS43_FLIP_Y : 0;
    xy_config |= config->switch_xy ? TPS43_SWITCH_XY_AXIS : 0;
    ret = tps43_i2c_write_reg8(dev, TPS43_REG_XY_CONFIG_0, xy_config);
    if (ret != 0) {
        LOG_WRN("Ошибка записи конфигурации XY: %d", ret);
        return ret;
    }

    // включение одиночных жестов на уровне железа
    if (config->single_tap || config->press_and_hold || config->swipes) {
        uint8_t single_gestures = 0;
        single_gestures |= config->single_tap ? TPS43_SINGLE_TAP : 0;
        single_gestures |= config->press_and_hold ? TPS43_PRESS_AND_HOLD : 0;
        single_gestures |= config->swipes ? TPS43_SWIPE_UP : 0;
        single_gestures |= config->swipes ? TPS43_SWIPE_DOWN : 0;
        single_gestures |= config->swipes ? TPS43_SWIPE_LEFT : 0;
        single_gestures |= config->swipes ? TPS43_SWIPE_RIGHT : 0;
        
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_SINGLE_FINGER_GESTURES, single_gestures);
        if (ret != 0) {
            LOG_WRN("Ошибка конфигурации одиночных жестов: %d", ret);
            return ret;
        }
        LOG_INF("Одиночные жесты включены: 0x%02X", single_gestures);
    }

    /* Press-and-hold hold time (reg 0x06BD, big-endian ms). The drag gesture
     * fires after TapTime + HoldTime, so a smaller HoldTime = faster drag.
     * Only written when press-and-hold is enabled (else the chip default
     * stands). */
    if (config->press_and_hold && config->hold_time_ms > 0) {
        ret = tps43_i2c_write_reg16(dev, TPS43_REG_HOLD_TIME, (uint16_t)config->hold_time_ms);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Hold time: %d", ret);
            return ret;
        }
        LOG_INF("Hold time установлен: %d ms", config->hold_time_ms);
    }

    // включение мульти-жестов
    if (config->two_finger_tap || config->scroll) {
        uint8_t multi_gestures = 0;
        multi_gestures |= config->two_finger_tap ? TPS43_TWO_FINGER_TAP : 0;
        multi_gestures |= config->scroll ? TPS43_SCROLL : 0;
        
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_MULTI_FINGER_GESTURES, multi_gestures);
        if (ret != 0) {
            LOG_WRN("Ошибка конфигурации мульти-жестов: %d", ret);
            return ret;
        }
        LOG_INF("Мульти-жесты включены: 0x%02X", multi_gestures);
    }

    /* Max simultaneous touches (reg 0x066A). Default is low (~2) -> a 3rd finger
     * trips TOO_MANY_FINGERS and clears XY. Raise it so 3-finger gestures get
     * data. Forced to >=3 when three-finger-drag is enabled. */
    uint8_t max_touch = config->max_multitouches;
    if (config->three_finger_drag && max_touch < 3) {
        max_touch = 3;
    }
    if (max_touch > 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_MAX_MULTITOUCH, max_touch);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Max multi-touches: %d", ret);
            return ret;
        }
        LOG_INF("Max multi-touches установлен: %d", max_touch);
    }

    /* Scroll initial distance (reg 0x06C8, big-endian px). The chip only starts
     * reporting SCROLL after the two fingers travel this far, so a smaller value
     * makes scroll engage on slighter movement. Only written when scroll is on
     * and a value is set (else chip default stands). */
    if (config->scroll && config->scroll_init_distance > 0) {
        ret = tps43_i2c_write_reg16(dev, TPS43_REG_SCROLL_INIT_DIST,
                                    (uint16_t)config->scroll_init_distance);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Scroll initial distance: %d", ret);
            return ret;
        }
        LOG_INF("Scroll initial distance установлен: %d px", config->scroll_init_distance);
    }

    /* ATI target (reg 0x056D, big-endian). The chip auto-tunes its touch gain to
     * this target; a higher value = more sensitive, to compensate for a glass/
     * overlay. A hardware reset reloads the NVM default, so we (re)write it here
     * every configure; AUTO_ATI below makes the new gain take effect. */
    if (config->ati_target > 0) {
        ret = tps43_i2c_write_reg16(dev, TPS43_REG_ATI_TARGET, (uint16_t)config->ati_target);
        if (ret != 0) {
            LOG_WRN("Ошибка записи ATI target: %d", ret);
            return ret;
        }
        LOG_INF("ATI target установлен: %d", config->ati_target);
    }

    // конфигурация фильтров
    ret = tps43_i2c_write_reg8(dev, TPS43_REG_FILTER_SETTINGS, config->filter_settings);
    if (ret != 0) {
        LOG_WRN("Ошибка записи настроек фильтров: %d", ret);
        return ret;
    }
    LOG_INF("Настройки фильтров установлены: 0x%02X", config->filter_settings);

    // установка признака завершения конфигурации
    ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONFIG_0, TPS43_SETUP_COMPLETE);
    if (ret != 0) {
        LOG_WRN("Ошибка записи флага завершения настройки: %d", ret);
        return ret;
    }

    /* Re-run ATI so a changed ATI target actually re-tunes the gain (the startup
     * ATI ran with the NVM-default target before we wrote ours). The AUTO_ATI bit
     * auto-clears when the routine finishes. */
    if (config->ati_target > 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONTROL_0, TPS43_AUTO_ATI);
        if (ret != 0) {
            LOG_WRN("Ошибка запуска AUTO_ATI: %d", ret);
        } else {
            LOG_INF("AUTO_ATI запущен (перетюнинг усиления под стекло)");
            k_sleep(K_MSEC(50));
        }
    }

    return 0;
}

/**
 * @brief Проверяет состояние сброса устройства и выполняет реконфигурацию
 * 
 * Ожидает готовности устройства после сброса, проверяет флаг SHOW_RESET
 * и отправляет подтверждение сброса (ACK_RESET) при необходимости.
 * Затем выполняет полную конфигурацию устройства.
 * 
 * @param dev Указатель на устройство тачпада
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int check_reset_and_reconfigure(const struct device *dev) {
    struct tps43_drv_data *drv_data = dev->data;
    int ret;
    uint8_t sys_info = 0;
    uint8_t wait_count = 0;
    const uint8_t max_wait_count = 50;

    // Ожидание готовности устройства
    do {
        ret = tps43_i2c_read_reg8(dev, TPS43_REG_SYSTEM_INFO_0, &sys_info);
        if (ret < 0) {
            k_sleep(K_MSEC(100));
            wait_count++;
            if (wait_count >= max_wait_count) {
                LOG_ERR("Устройство не отвечает после %d мс", wait_count * 100);
                return -ETIMEDOUT;
            }
        }
    } while (ret < 0);
    
    LOG_INF("Устройство готово через %d мс", wait_count * 100);

    // после сброса устанавливаем флаг на подтверждение что сброс был выполнен
    if (sys_info & TPS43_SHOW_RESET) {
        LOG_INF("Обнаружен SHOW_RESET, отправка ACK_RESET");
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONTROL_0, TPS43_ACK_RESET);
        if (ret != 0) {
            LOG_ERR("Ошибка отправки ACK_RESET: %d", ret);
            return ret;
        }
        k_sleep(K_MSEC(10));
    }

    ret = tps43_configure_device(dev);
    if (ret != 0) {
        LOG_ERR("Ошибка конфигурации устройства: %d", ret);
        return ret;
    }

    drv_data->device_ready = true;
    
    return 0;
}

/**
 * @brief Публичная функция для перевода тачпада в suspend/resume
 * 
 * @param dev Указатель на устройство тачпада
 * @param suspend true - перевести в suspend, false - вывести из suspend
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_set_suspend(const struct device *dev, bool suspend) {
    return tps43_set_suspend_internal(dev, suspend, false);
}

/**
 * @brief Инициализирует драйвер тачпада TPS43
 * 
 * Выполняет полную инициализацию драйвера: проверяет доступность I2C шины,
 * выполняет аппаратный сброс через GPIO RST (если подключен), ожидает готовности
 * устройства, конфигурирует регистры тачпада и настраивает прерывания GPIO RDY.
 * Также инициализирует систему управления питанием при необходимости.
 * 
 * @param dev Указатель на устройство тачпада
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_init(const struct device *dev) {

    struct tps43_drv_data *drv_data = dev->data;
    const struct tps43_config *config = dev->config;
    int ret;

    drv_data->dev = dev;

    LOG_INF("=== Драйвер Azoteq tps43 для устройства %s ===", dev->name);
    
    // Проверка I2C шины
    if (!device_is_ready(config->i2c_bus.bus)) {
        LOG_ERR("Шина I2C не доступна");
        return -ENODEV;
    }
    
    LOG_INF("I2C шина: %s", config->i2c_bus.bus->name);
    LOG_INF("I2C адрес: 0x%02x", config->i2c_bus.addr);

    ret = tps43_reset_values(dev);
    if (ret != 0) {
        LOG_ERR("Ошибка сброса значений: %d", ret);
        return ret;
    }

    // GPIO сброс через hardware RST
    if (config->rst_gpio.port) {
        ret = gpio_pin_configure_dt(&config->rst_gpio, GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            LOG_ERR("Ошибка конфигурации RST GPIO: %d", ret);
            return ret;
        }
        
        gpio_pin_set_dt(&config->rst_gpio, 0);
        k_sleep(K_MSEC(10));
        gpio_pin_set_dt(&config->rst_gpio, 1);
        k_sleep(K_MSEC(610));
        
        LOG_INF("Аппаратный сброс завершен");
    }

    // проверка SHOW_RESET и конфигурация
    ret = check_reset_and_reconfigure(dev);
    if (ret != 0) {
        LOG_ERR("Ошибка конфигурации устройства: %d", ret);
        return ret;
    }

    // прерывания RDY настраиваем только ПОСЛЕ конфигурации устройства!
    if (config->rdy_gpio.port != NULL) {
        ret = gpio_pin_configure_dt(&config->rdy_gpio, GPIO_INPUT);
        if (ret != 0) {
            LOG_WRN("Ошибка конфигурации RDY GPIO: %d", ret);
        } else {
            ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, 
                                                    GPIO_INT_EDGE_TO_ACTIVE);
            if (ret == 0) {
                gpio_init_callback(&drv_data->rdy_cb, tps43_rdy_callback, 
                                    BIT(config->rdy_gpio.pin));
                ret = gpio_add_callback(config->rdy_gpio.port, &drv_data->rdy_cb);
                if (ret == 0) {
                    LOG_INF("Прерывание RDY сконфигурировано");
                } else {
                    LOG_WRN("Ошибка добавления callback RDY: %d", ret);
                }
            }
        }
    }

    drv_data->initialized = true;
    drv_data->suspended = false;

    // Инициализируем семафор для защиты операций I2C
    // Первый параметр - начальное количество (1 = доступен)
    // Второй параметр - максимальное количество (1 = бинарный семафор)
    k_sem_init(&drv_data->lock, 1, 1);

    k_work_init(&drv_data->work, tps43_work_handler);
    
    LOG_INF("Драйвер TPS43 успешно инициализирован");
    return 0;
}

 
#define TPS43_INIT(inst)                                                                             \
    static struct tps43_drv_data tps43_##inst##_drvdata = {                                          \
        .device_ready = false,                                                                       \
        .initialized = false,                                                                        \
        .scroll_active = false,                                                                      \
        .drag_active = false,                                                                        \
        .suspended = false,                                                                          \
        .last_tap_time = 0,                                                                          \
        .prev_num_fingers = 0,                                                                       \
        .three_drag_active = false,                                                                  \
        .scroll_accum_x = 0,                                                                         \
        .scroll_accum_y = 0,                                                                         \
    };                                                                                               \
                                                                                                     \
    static const struct tps43_config tps43_##inst##_config = {                                       \
        .i2c_bus = I2C_DT_SPEC_INST_GET(inst),                                                       \
        .rdy_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, rdy_gpios, {0}),                                  \
        .rst_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, rst_gpios, {0}),                                  \
        .single_tap = DT_INST_PROP(inst, single_tap),                                                \
        .press_and_hold = DT_INST_PROP(inst, press_and_hold),                                        \
        .hold_time_ms = DT_INST_PROP_OR(inst, hold_time_ms, 300),                                    \
        .tap_drag = DT_INST_PROP(inst, tap_drag),                                                    \
        .tap_drag_window_ms = DT_INST_PROP_OR(inst, tap_drag_window_ms, 300),                        \
        .three_finger_drag = DT_INST_PROP(inst, three_finger_drag),                                  \
        .max_multitouches = DT_INST_PROP_OR(inst, max_multitouches, 0),                              \
        .two_finger_tap = DT_INST_PROP(inst, two_finger_tap),                                        \
        .scroll = DT_INST_PROP(inst, scroll),                                                        \
        .swipes = DT_INST_PROP(inst, swipes),                                                        \
        .invert_x = DT_INST_PROP(inst, invert_x),                                                    \
        .invert_y = DT_INST_PROP(inst, invert_y),                                                    \
        .switch_xy = DT_INST_PROP(inst, switch_xy),                                                  \
        .invert_scroll_x = DT_INST_PROP(inst, invert_scroll_x),                                      \
        .invert_scroll_y = DT_INST_PROP(inst, invert_scroll_y),                                      \
        .sensitivity = DT_INST_PROP_OR(inst, sensitivity, 100),                                      \
        .scroll_sensitivity = DT_INST_PROP_OR(inst, scroll_sensitivity, 50),                         \
        .scroll_init_distance = DT_INST_PROP_OR(inst, scroll_initial_distance, 0),                   \
        .ati_target = DT_INST_PROP_OR(inst, ati_target, 0),                                          \
        .enable_power_management = DT_INST_PROP_OR(inst, enable_power_management, true),             \
        .filter_settings = DT_INST_PROP_OR(inst, filter_settings, 0x0F),                             \
    };                                                                                               \
                                                                                                     \
    DEVICE_DT_INST_DEFINE(inst, tps43_init, NULL, &tps43_##inst##_drvdata, &tps43_##inst##_config,   \
                        POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);                              \
    BUILD_ASSERT(DT_INST_REG_ADDR(inst) == TPS43_I2C_ADDR, "Несоответствие адреса I2C");


DT_INST_FOREACH_STATUS_OKAY(TPS43_INIT)

/**
 * @brief Публичная функция для управления режимом сна тачпада
 * 
 * Эта функция используется системой управления питанием ZMK (через tps43_idle_sleeper)
 * для перевода тачпада в режим сна при переходе клавиатуры в состояние idle/sleep.
 * 
 * @param dev Указатель на устройство тачпада
 * @param sleep true - перевести в режим сна, false - пробудить
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
int tps43_set_sleep(const struct device *dev, bool sleep) {
    if (dev == NULL) {
        return -EINVAL;
    }
    return tps43_set_suspend(dev, sleep);
}
