// Lightweight NimBLE HOGP central; no emulator or second Bluetooth stack.
// Reference: ESP-IDF nimble/blecent; Q36 Xbox BLE mode from gameboy/fofo_radio.cpp.
#include "sdkconfig.h"
#include "charm.h"
#include "charm_hid.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
static const char *TAG="charm.pad";
static bool started,wanted,known;
static ble_addr_t known_addr;
static uint8_t own;
static uint16_t conn=BLE_HS_CONN_HANDLE_NONE,svc_end,map_handle;
static int64_t until;
static charm_hid_map_t map;
static uint8_t descriptor[2048];static size_t descriptor_len;
static struct {uint16_t value,end,ccc,ref;uint8_t id,type;} reports[8];
static unsigned count,index_report;
static int gap(struct ble_gap_event *e,void *arg);
static void scan(void) {
    if(!wanted || conn!=BLE_HS_CONN_HANDLE_NONE || (!known && esp_timer_get_time()>=until)) {charm_pad_status(conn!=BLE_HS_CONN_HANDLE_NONE,false);return;}
    struct ble_gap_disc_params p={.passive=0,.filter_duplicates=1,.itvl=0x60,.window=0x30};
    int rc=ble_gap_disc(own,known?BLE_HS_FOREVER:(until-esp_timer_get_time())/1000,&p,gap,NULL);
    charm_pad_status(false,rc==0 && esp_timer_get_time()<until);ESP_LOGI(TAG,"BLE scan rc=%d",rc);
}
static int subscribed(uint16_t c,const struct ble_gatt_error *err,struct ble_gatt_attr *attr,void *arg);
static void next_report(void);
static int reference(uint16_t c,const struct ble_gatt_error *err,struct ble_gatt_attr *attr,void *arg) {
    (void)c;(void)arg;uint8_t value[2];
    if(!err->status && attr && OS_MBUF_PKTLEN(attr->om)>=2 && !os_mbuf_copydata(attr->om,0,2,value)) {
        reports[index_report].id=value[0];reports[index_report].type=value[1];
    }
    if(reports[index_report].ccc && reports[index_report].type==1) {
        uint8_t enable[2]={1,0};
        if(!ble_gattc_write_flat(conn,reports[index_report].ccc,enable,2,subscribed,NULL)) return 0;
    }
    index_report++;next_report();return 0;
}
static int subscribed(uint16_t c,const struct ble_gatt_error *err,struct ble_gatt_attr *attr,void *arg) {
    (void)c;(void)attr;(void)arg;
    if(!err->status) {
        struct ble_gap_conn_desc desc;
        if(!known && !ble_gap_conn_find(conn,&desc)) {
            known_addr=desc.peer_id_addr;known=true;char saved[64];
            snprintf(saved,sizeof(saved),"%u:%02x%02x%02x%02x%02x%02x",known_addr.type,
                known_addr.val[0],known_addr.val[1],known_addr.val[2],known_addr.val[3],known_addr.val[4],known_addr.val[5]);
            config_set_str("charm_pad",saved);
        }
        until=0;charm_pad_status(true,false);ESP_LOGI(TAG,"input report %u subscribed",reports[index_report].id);}
    index_report++;next_report();return 0;
}
static int dsc(uint16_t c,const struct ble_gatt_error *err,uint16_t chr,const struct ble_gatt_dsc *d,void *arg) {
    (void)c;(void)chr;(void)arg;
    if(!err->status && d) {
        uint16_t uuid=ble_uuid_u16(&d->uuid.u);
        if(uuid==0x2902) reports[index_report].ccc=d->handle;
        if(uuid==0x2908) reports[index_report].ref=d->handle;
    } else {
        if(err->status==BLE_HS_EDONE && reports[index_report].ref &&
            !ble_gattc_read(conn,reports[index_report].ref,reference,NULL)) return 0;
        index_report++;next_report();
    }
    return 0;
}
static void next_report(void) {
    if(index_report>=count) return;
    if(ble_gattc_disc_all_dscs(conn,reports[index_report].value,reports[index_report].end,dsc,NULL)) {index_report++;next_report();}
}
static int read_map(uint16_t c,const struct ble_gatt_error *err,struct ble_gatt_attr *attr,void *arg) {
    (void)c;(void)arg;
    if(!err->status && attr) {
        size_t n=OS_MBUF_PKTLEN(attr->om);
        if(descriptor_len+n>sizeof(descriptor)) {ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);return BLE_HS_EINVAL;}
        os_mbuf_copydata(attr->om,0,n,descriptor+descriptor_len);descriptor_len+=n;
    } else if(err->status==BLE_HS_EDONE) {
        if(charm_hid_parse(&map,descriptor,descriptor_len)) {index_report=0;next_report();}
        else {ESP_LOGW(TAG,"unsupported HID map");ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);}
    }
    return 0;
}
static int chr(uint16_t c,const struct ble_gatt_error *err,const struct ble_gatt_chr *ch,void *arg) {
    (void)c;(void)arg;
    if(!err->status && ch) {
        if(count && reports[count-1].end==svc_end) reports[count-1].end=ch->def_handle-1;
        uint16_t uuid=ble_uuid_u16(&ch->uuid.u);
        if(uuid==0x2a4b) map_handle=ch->val_handle;
        if(uuid==0x2a4d && (ch->properties & BLE_GATT_CHR_PROP_NOTIFY) && count<8) {
            reports[count].value=ch->val_handle;reports[count].end=svc_end;count++;
        }
    } else if(err->status==BLE_HS_EDONE && map_handle) {
        descriptor_len=0;ble_gattc_read_long(conn,map_handle,0,read_map,NULL);
    }
    return 0;
}
static int service(uint16_t c,const struct ble_gatt_error *err,const struct ble_gatt_svc *s,void *arg) {
    (void)arg;
    if(!err->status && s) {svc_end=s->end_handle;count=map_handle=0;memset(reports,0,sizeof(reports));ble_gattc_disc_all_chrs(c,s->start_handle,s->end_handle,chr,NULL);}
    return 0;
}
static void discover(void) {ble_uuid16_t uuid=BLE_UUID16_INIT(0x1812);ble_gattc_disc_svc_by_uuid(conn,&uuid.u,service,NULL);}
static int gap(struct ble_gap_event *e,void *arg) {
    (void)arg;
    switch(e->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;if(ble_hs_adv_parse_fields(&fields,e->disc.data,e->disc.length_data)) break;
        char name[40]={0};size_t n=fields.name_len<39?fields.name_len:39;
        if(fields.name) for(size_t i=0;i<n;i++) name[i]=tolower(fields.name[i]);
        bool remembered=known && !memcmp(&known_addr,&e->disc.addr,sizeof(known_addr));
        if(!remembered && esp_timer_get_time()>=until) break;
        bool hid=false;
        for(unsigned i=0;i<fields.num_uuids16;i++) if(ble_uuid_u16(&fields.uuids16[i].u)==0x1812) hid=true;
        bool gamepad=fields.appearance_is_present && (fields.appearance==0x03c4 || fields.appearance==0x03c3);
        if(name[0]) ESP_LOGI(TAG,"scan name=%s hid=%d appearance=%04x",name,hid,fields.appearance);
        if(!remembered && !hid && !gamepad && !strstr(name,"xbox") && !strstr(name,"q36") && !strstr(name,"shanwan") && !strstr(name,"gamepad") && !strstr(name,"controller")) break;
        ble_gap_disc_cancel();int rc=ble_gap_connect(own,&e->disc.addr,15000,NULL,gap,NULL);
        ESP_LOGI(TAG,"Q36-compatible controller connect rc=%d",rc);if(rc) scan();break;
    }
    case BLE_GAP_EVENT_CONNECT:
        if(e->connect.status) {scan();break;}
        conn=e->connect.conn_handle;
        if(ble_gap_security_initiate(conn)) discover();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if(!e->enc_change.status) discover();else ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t data[64];size_t n=OS_MBUF_PKTLEN(e->notify_rx.om);
        if(n>sizeof(data)) break;
        os_mbuf_copydata(e->notify_rx.om,0,n,data);
        for(unsigned i=0;i<count;i++) if(reports[i].value==e->notify_rx.attr_handle) {
            uint16_t buttons;int hat;
            if(charm_hid_input(&map,reports[i].id,data,n,&buttons,&hat)) {
                charm_pad_input(buttons,hat);ESP_LOGD(TAG,"buttons=%04x hat=%d",buttons,hat);
            }
        }break;
    }
    case BLE_GAP_EVENT_DISCONNECT:conn=BLE_HS_CONN_HANDLE_NONE;charm_pad_status(false,false);scan();break;
    case BLE_GAP_EVENT_DISC_COMPLETE:charm_pad_status(false,false);if(known && wanted) scan();break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    return 0;
}
static void sync_host(void) {if(!ble_hs_id_infer_auto(0,&own)) scan();}
static void host(void *arg) {(void)arg;nimble_port_run();nimble_port_freertos_deinit();}
extern void ble_store_config_init(void);
void charm_controller_pair(void) {
    if(!config_setup_complete() || !config_is_provisioned()) {ESP_LOGW(TAG,"finish Muse pairing first");return;}
    wanted=true;
    until=esp_timer_get_time()+180000000;
    if(started) {ble_gap_disc_cancel();scan();return;}
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if(nimble_port_init()!=ESP_OK) {ESP_LOGW(TAG,"Bluetooth init failed");return;}
    started=true;ble_svc_gap_init();ble_svc_gatt_init();ble_svc_gap_device_name_set("Muse Charm");
    ble_hs_cfg.sync_cb=sync_host;ble_hs_cfg.sm_bonding=1;ble_hs_cfg.sm_io_cap=BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();nimble_port_freertos_init(host);
}
void charm_controller_disconnect(void) {
    wanted=false;until=0;if(!started) return;ble_gap_disc_cancel();
    if(conn!=BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);
    charm_pad_status(false,false);
}

void charm_controller_resume(void) {
    char saved[64];unsigned type,v[6];
    if(!config_get_str("charm_pad",saved,sizeof(saved)) || sscanf(saved,"%u:%2x%2x%2x%2x%2x%2x",&type,&v[0],&v[1],&v[2],&v[3],&v[4],&v[5])!=7 || type>1) return;
    known_addr.type=type;for(int i=0;i<6;i++) known_addr.val[i]=v[i];known=true;
    charm_controller_pair();until=0;
}
