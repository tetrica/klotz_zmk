# Vendored Azoteq TPS43 trackpad driver (ported from the Sofle build).
# Source in drivers/input/, bindings in dts/bindings/input/. Compiled only when
# CONFIG_INPUT_TPS43=y (set in config/klotz.conf). This repo is itself a Zephyr
# module (zephyr/module.yml) so ZMK's build (ZMK_EXTRA_MODULES) picks it up.
if(CONFIG_INPUT_TPS43)
  zephyr_library()
  zephyr_library_sources(drivers/input/tps43.c)
  # ZMK activity listener (puts the pad to sleep with the keyboard) must live in
  # the app image.
  target_sources(app PRIVATE drivers/input/tps43_idle_sleeper.c)
endif()
