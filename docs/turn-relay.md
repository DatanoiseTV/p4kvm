# WebRTC TURN relay (coturn)

The device's WebRTC transport (H.264 video + HID data channel) needs a working
ICE candidate pair between the browser and the device. On some networks a direct
pair never forms:

- **Same LAN behind one NAT** — the only mutually visible candidates are the
  STUN-reflexive (public) ones, which need router hairpinning; most home routers
  refuse it.
- **Station isolation** — many APs block client-to-client traffic, so the
  browser can't reach the device's LAN IP even though both are "on the network".
- **Off-LAN / remote access** — the browser isn't on the device's network at all.

A TURN relay fixes all three: both ends connect out to the relay and it forwards
the media. It's also what enables reaching the KVM **from anywhere**, not just
the local network.

The device uses coturn's `use-auth-secret` mode (RFC 5766 "TURN REST API"): it
stores only a shared secret and derives short-lived HMAC credentials on demand,
so no long-term password is ever exposed to the browser.

## 1. Install coturn on a public host

On the public box (e.g. the same server as the WireGuard endpoint):

```sh
sudo apt install coturn          # Debian/Ubuntu
```

Generate a strong secret and keep it (you'll paste it into the device config):

```sh
openssl rand -hex 32
```

## 2. Configure `/etc/turnserver.conf`

```conf
listening-port=3478
fingerprint

# REST-API / ephemeral credentials. No user database; the device derives
# time-limited credentials from this secret via HMAC-SHA1.
use-auth-secret
static-auth-secret=PASTE_THE_openssl_rand_SECRET_HERE
realm=p4kvm

# Public address of this host. If the box is itself behind NAT, set external-ip
# to the public IP and listening-ip/relay-ip to the private one.
listening-ip=YOUR_PUBLIC_IP
relay-ip=YOUR_PUBLIC_IP
# external-ip=PUBLIC_IP/PRIVATE_IP   # only if the host is behind NAT

# Relay port range (open these in the firewall too).
min-port=49152
max-port=65535

# Hardening: this relay only ever forwards between the browser and the device's
# public reflexive addresses, so refuse to relay to private/loopback ranges.
# That stops the relay being abused to reach internal services (SSRF) or as an
# open proxy.
no-multicast-peers
no-cli
denied-peer-ip=0.0.0.0-0.255.255.255
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255
denied-peer-ip=::1
denied-peer-ip=fc00::-fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff
denied-peer-ip=fe80::-febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff
```

Enable and start it:

```sh
sudo sed -i 's/^#\?TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/default/coturn
sudo systemctl enable --now coturn
```

## 3. Open the firewall

```sh
sudo ufw allow 3478/udp
sudo ufw allow 49152:65535/udp
```

(Add `3478/tcp` and, if you configure TLS on 5349, `5349/tcp` for TCP/TURNS
fallback on restrictive networks.)

## 4. Point the device at it

In the web UI: **Panel → SETUP**:

- **TURN relay URL** — `turn:YOUR_PUBLIC_IP:3478`
- **TURN secret** — the `static-auth-secret` from step 1

Save & restart. The device stores these in NVS (never in the firmware image or
git). On the next WebRTC attempt it advertises a relay candidate and hands the
browser the same server with fresh credentials via `GET /webrtc/ice`.

## 5. Verify

Load the console with logging: `http://<device>/?webrtclog=1`, open the browser
console. You should see an `iceServers` line that includes the `turn:` URL, and
the transport in the diagnostics panel should flip to **WebRTC · H.264**. On the
device serial log you'll see `TURN relay enabled: turn:...`.

If it still won't connect, confirm UDP 3478 and the relay port range are
reachable from both the browser and the device (a stateful firewall dropping the
relay range is the usual culprit), and that the device clock is set — the
credentials are time-based, so SNTP must have synced (the boot log shows
`time synced` once WireGuard/NTP is up).

## Security notes

- The `static-auth-secret` lives only on the public host and in the device's
  NVS. The browser only ever receives an ephemeral `username`/`credential` pair
  that expires (default 1 hour).
- The `denied-peer-ip` block list is not optional — without it an open TURN
  relay is an abuse magnet.
- Keep the KVM itself behind its HTTP auth and the WireGuard tunnel as before;
  the TURN relay carries only the WebRTC media, not access control.
