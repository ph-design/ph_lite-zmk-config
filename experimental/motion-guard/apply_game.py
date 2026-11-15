from pathlib import Path
import sys
r=Path(sys.argv[1]);
p=r/"app/src/usb_hid.c";s=p.read_text();s="#include <zmk/imu_game.h>\n#include <zmk/axis_descriptor.h>\n#include <zephyr/sys/byteorder.h>\n"+s
s=s.replace("static const struct device *hid_dev;","static const struct device *hid_dev;\n#if IS_ENABLED(CONFIG_ZMK_MOTION)\nstatic uint8_t axis_report[3]={4,0,0},axis_transfer[3]={4,0,0};\nstatic const uint8_t axis_descriptor[]={IMU_AXIS_DESCRIPTOR(4)};\nstatic uint8_t usb_game_descriptor[sizeof(zmk_hid_report_desc)+sizeof(axis_descriptor)];\n#endif")
s=s.replace("    case HID_REPORT_TYPE_INPUT:\n        switch", "    case HID_REPORT_TYPE_INPUT:\n#if IS_ENABLED(CONFIG_ZMK_MOTION)\n        if((setup->wValue & HID_GET_REPORT_ID_MASK)==4){*data=axis_report;*len=3;return 0;}\n#endif\n        switch")
s=s.replace("usb_hid_register_device(hid_dev, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &ops);", """#if IS_ENABLED(CONFIG_ZMK_MOTION)
    memcpy(usb_game_descriptor,zmk_hid_report_desc,sizeof(zmk_hid_report_desc));
    memcpy(usb_game_descriptor+sizeof(zmk_hid_report_desc),axis_descriptor,sizeof(axis_descriptor));
    usb_hid_register_device(hid_dev,usb_game_descriptor,sizeof(usb_game_descriptor),&ops);
#else
    usb_hid_register_device(hid_dev,zmk_hid_report_desc,sizeof(zmk_hid_report_desc),&ops);
#endif""")
pos=s.index("int zmk_usb_hid_send_keyboard_report(");s=s[:pos]+"""#if IS_ENABLED(CONFIG_ZMK_MOTION)
int zmk_usb_hid_send_axis(int16_t axis) {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if(hid_protocol!=HID_PROTOCOL_REPORT)return -ENOTSUP;
#endif
    sys_put_le16((uint16_t)axis,axis_report+1);
    if(!hid_dev || zmk_usb_get_status()!=USB_DC_CONFIGURED)return -ENODEV;
    if(k_sem_take(&hid_sem,K_NO_WAIT))return -EAGAIN;
    memcpy(axis_transfer,axis_report,sizeof(axis_report));
    int rc=hid_int_ep_write(hid_dev,axis_transfer,sizeof(axis_transfer),NULL);
    if(rc){k_sem_give(&hid_sem);}
    return rc;
}
#endif

"""+s[pos:];p.write_bytes(s.encode())
p=r/"app/src/2g4.c";s=p.read_text();pos=s.index("int zmk_2g4_send_keyboard_report(");s=s[:pos]+"""#if IS_ENABLED(CONFIG_ZMK_MOTION)
int zmk_2g4_send_axis(int16_t axis){
    if(!ready)return -ENODEV;
    struct zmk_esb_payload payload={.pipe=0,.noack=false};payload.data[0]=0xd1;
    sys_put_le16((uint16_t)axis,payload.data+1);
    int n=zmk_2g4_crypto_encrypt(payload.data,3,CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH);
    if(n<0){return n;}
    payload.length=n;
    return zmk_esb_write_payload(&payload); /* Never flush key reports for an axis. */
}
#endif

"""+s[pos:];p.write_bytes(s.encode())
p=r/"app/src/motion.c";s="#include <zmk/imu_game.h>\n"+p.read_text();s=s.replace("int zmk_motion_set_live_stream(bool on) {","int zmk_motion_set_live_stream(bool on) {\n    zmk_imu_game_view(on);").replace("#define LIVE_PERIOD_MS 100","#define LIVE_PERIOD_MS 40");p.write_bytes(s.encode())
