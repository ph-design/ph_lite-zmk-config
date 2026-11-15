#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/settings/settings.h>
#include <esb.h>
#include <zmk/2g4.h>
#include "hub.h"
#include "ble-mouse.h"
#include "hub-peer.h"
#include "diagnostics.h"
int main(void) {
    int err = bt_enable(NULL);
    if (err) return err;
    err = settings_load();
    if (err) return err;
    hub_peer_load_finish();
    err=hub_usb_init();if(err)return err;
    err=ble_mouse_init();if(err)return err;
    /* Keep USB diagnostics available after a fault without repeatedly writing
     * crypto epochs to flash in a reboot loop. AT+RST explicitly retries. */
    if(!hub_diagnostics_safe_mode())hub_radio_error=zmk_2g4_dongle_start();
    hub_radio_run();
    return 0;
}
