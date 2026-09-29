#pragma once

/*
 * mDNS / DNS-SD discovery (doc/mdns-discovery.md). In station mode the device
 * advertises one Bonjour service next to the UDP multicast GET_INFO scan
 * (doc/udp-discovery.md), which keeps working unchanged:
 *
 *   service   _cything._tcp on cy_tcp_port — FIXED, never per channel: iOS
 *             can only browse types listed in the app's Info.plist
 *   host      cything-<unit suffix>.local, e.g. cything-a1b2c3.local
 *   instance  "Cything A1B2C3 #<channel tag>" ("Cything A1B2C3" if no tag)
 *   TXT       id=<deviceId> ch=<channel tag> v=<fw version> type=<deviceType>
 *             paired=0|1 pw=0|1 prov=0|1
 *
 * The channel tag is cy_mdns_channel_tag (device_config/cy_config.h), set per
 * channel by the sketch the app exports. The app filters on it; nothing in TXT
 * is secret — anyone on the Wi-Fi can read it.
 *
 * Lifecycle: the responder is started once, after the first
 * IP_EVENT_STA_GOT_IP, and never torn down. From then on the mdns component
 * itself follows the station: WIFI_EVENT_STA_DISCONNECTED disables its STA
 * sockets (nothing is announced with a stale address), IP_EVENT_STA_GOT_IP
 * re-enables them and re-probes/announces with the new address. Not started
 * in pairing (soft-AP) mode. All entry points are safe from any task.
 */

/* Create the lock. Call once from cything_begin(), before any task can call
 * mdns_discovery_refresh(). */
void mdns_discovery_init(void);

/* Start the responder and register the service. Call from a task with a
 * normal stack once the station has an IP (wifi_init_sta after
 * WIFI_CONNECTED_BIT), never from the esp_event task. Idempotent. Failures are
 * logged; the device then has UDP discovery only. */
void mdns_discovery_start(void);

/* Station got / lost its address (esp_event task). Only logs — the mdns
 * component handles the sockets itself, see above. */
void mdns_discovery_on_ip(void);
void mdns_discovery_on_ip_lost(void);

/* Pairing, password or provisioning state changed: rebuild the TXT record and
 * re-announce it. No-op until started. Must not be called with the
 * paired_list or device_password lock held. */
void mdns_discovery_refresh(void);
