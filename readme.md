<picture>
  <source media="(prefers-color-scheme: dark)" srcset="/docs/images/KLOTZ_font_dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="/docs/images/KLOTZ_font_bright.svg">
  <img alt="KLOTZ logo font" src="/docs/images/KLOTZ_font_bright.svg">
</picture>

# ZMK CONFIG FOR THE KLOTZ SPLIT KEYBOARD

[Here](https://github.com/GEIGEIGEIST/klotz) you can find the hardware files and build guide for the KLOTZ.

KLOTZ is a 34 key column-staggered split keyboard running [ZMK](https://zmk.dev/). It supports a low profile encoder and three status LEDs on every side.

![KLOTZ layout](/docs/images/KLOTZ_layout.svg)
![KLOTZ layout](/docs/images/Keymap.svg)

## KEYMAP MACROS

The macros below are defined in [`config/klotz.keymap`](/config/klotz.keymap).

### Active macros

| Macro | Output | Trigger |
| --- | --- | --- |
| `tabcw` | `Alt+Tab` | `W+E` combo or clockwise left encoder rotation on the `SYM`, `fn`, or `adjust_mouse` layer |
| `tabccw` | `Alt+Shift+Tab` | Counter-clockwise left encoder rotation on the `SYM`, `fn`, or `adjust_mouse` layer |
| `browser_tab_next` | `Ctrl+Tab` | Clockwise right encoder rotation on the `fn` or `adjust_mouse` layer |
| `browser_tab_previous` | `Ctrl+Shift+Tab` | Counter-clockwise right encoder rotation on the `fn` or `adjust_mouse` layer |
| `macro_ctrlz` | `Ctrl+Z` | `Z+X` combo on a base layer |
| `macro_ctrlshftz` | `Ctrl+Shift+Z` | `Z+X+C` combo on a base layer |
| `macro_ctrlx` | `Ctrl+X` | `X+C` combo on a base layer |
| `macro_ctrlc` | `Ctrl+C` | `C+V` combo on a base layer |
| `macro_ctrlv` | `Ctrl+V` | `V+B` combo on a base layer |
| `macro_up` | `Ctrl+U` | Physical `X` key on the `num` layer |
| `macro_down` | `Ctrl+D` | Physical `V` key on the `num` layer |
| `omarchy_close` | `Super+Q` | Physical `Q` key on the `nav` layer |
| `omarchy_menu` | `Super+Space` | Physical `W` key on the `nav` layer |
| `omarchy_terminal` | `Super+Enter` | Physical `E` key on the `nav` layer |
| `omarchy_browser` | `Super+Shift+Enter` | Physical `R` key on the `nav` layer |
| `omarchy_fullscreen` | `Super+F` | Physical `T` key on the `nav` layer |
| `omarchy_cut` | `Super+X` | Physical `X` key on the `nav` layer |
| `omarchy_copy` | `Super+C` | Physical `C` key on the `nav` layer |
| `omarchy_paste` | `Super+V` | Physical `V` key on the `nav` layer |

### Defined but currently unused

| Macro | Programmed output |
| --- | --- |
| `macro_esc` | Return to the base layer and send `Escape` |
| `macro_ctrls` | `Ctrl+S` |
| `os_hk` | Parameterized `Alt+GUI+Shift` shortcut |
| `m_set` | Send `M`, followed by `Shift` plus its parameter |
| `m_jmp` | Send grave accent, followed by `Shift` plus its parameter |
| `m_paste` | Send double quote, its parameter, and `P` |
| `m_dkp` | Send its two parameters as consecutive key taps |


## HOW TO USE

- fork this repo
- `git clone` your repo, to create a local copy on your PC (you can use the [command line](https://www.atlassian.com/git/tutorials) or [github desktop](https://desktop.github.com/))
- adjust the klotz.keymap file (find all the keycodes on [the zmk docs pages](https://zmk.dev/docs/codes/))
- `git push` your repo to your fork
- on the GitHub page of your fork navigate to "Actions"
- scroll down and unzip the `firmware.zip` archive that contains the latest firmware
- connect the left half of the KLOTZ to your PC, press reset twice
- the keyboard should now appear as a mass storage device
- drag'n'drop the `klotz_left-nice_nano_v2-zmk.uf2` file from the archive onto the storage device
- repeat this process with the right half and the `klotz_right-nice_nano_v2-zmk.uf2` file.




