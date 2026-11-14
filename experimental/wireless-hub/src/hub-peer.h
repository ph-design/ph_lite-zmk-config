#pragma once
#include <zmk/2g4_crypto.h>
#define HUB_PEERS 2
unsigned hub_peer_target(void);
int hub_peer_select(unsigned slot);
int hub_peer_select_session(unsigned slot);
bool hub_peer_enabled(unsigned slot);
bool hub_peer_paired(unsigned slot);
void hub_peer_addr(unsigned slot,struct zmk_2g4_addr *out);
uint32_t hub_peer_key_id(unsigned slot);
int hub_peer_pair(unsigned slot,const uint8_t key[16],const struct zmk_2g4_addr *addr);
int hub_peer_clear(unsigned slot);
int hub_peer_encrypt(unsigned slot,uint8_t *data,size_t len,size_t cap);
int hub_peer_decrypt(unsigned slot,uint8_t *data,size_t len);
void hub_peer_load_finish(void);
int hub_radio_reconfigure(void);
void hub_radio_release(unsigned slot);
void hub_peer_status(char *out,size_t size);
bool hub_any_keyboard_connected(void);
int hub_lowest_battery(void);
bool hub_keyboard_connected(unsigned slot);
int hub_keyboard_battery(unsigned slot);
void hub_keyboard_stats(unsigned slot,uint32_t *rx,uint32_t *fail,uint32_t *drop);
bool studio_radio_busy(void);
