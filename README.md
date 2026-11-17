# <img width="72" height="72" alt="beeper114" src="https://github.com/user-attachments/assets/9658bb61-5c5c-4d61-b045-c036e2fdeac1" align="center" /> Berry Bridge

**Berry Bridge** is a native unofficial Beeper client for rooted BlackBerry 10 devices that synchronizes your messaging accounts via the Beeper Desktop API.

## 🍴 About this fork

This is a fork of [adem-zengin/BerryBridge](https://github.com/adem-zengin/BerryBridge) with a few additions. It installs as a separate app (**Berry Bridge**, package `it.berrybridge.app`, downloaded media in `shared/misc/BerryBridge`), so it can sit next to the original; uninstall the original once this one works, or you will get every notification twice.

It follows the original up to **1.2.4** (new compose page with the account's contacts and the phone's, emoji packs, reply gesture, avatar preview in the chat list, chat title bar with the avatar, *Accounts* page split from *Settings*, BlackBerry Classic trackpad option).

**Install**: download the `.bar` from [Releases](https://github.com/damianolampisti00/BerryBridge/releases). It is not signed by BlackBerry, so it needs a rooted phone (`tools/installbar.sh` over SSH, as `package.ps1 -Install` does) or a sideloading tool such as DBBT.

What it adds:

- **Your messages stay on the server**: chats, messages, avatars and contacts live in a cache in the app's private folder, filled from Beeper Desktop when the app opens (the chat list in a few seconds, messages page by page as you scroll) and deleted when it closes. The original keeps them in `shared/misc`, which other apps can read. With the app closed, the service still shows notifications, asking Beeper for the chat's name.
- **Battery**: no reconnect loop on the push link (the original opened a new TLS connection every few seconds), no wake-ups while the app is minimized, and new messages don't reload the chat lists until you come back to the app.
- **Network trouble**: a service that loses its network (the memory cap of headless apps, a DNS server left over from another Wi-Fi) restarts itself and reconnects. TLS certificates are verified (except for servers on the local network).

- **Notifications with Instant Preview**, and tapping one opens that chat (the original does this too since 1.2.3). Here it also works for chats further down than the part of the list loaded so far (they are looked up in the database), switches to the account's tab without rebuilding the tabs, leaves an already open chat alone, and waits until the app is ready when the tap starts it.
- **Profile pictures** for direct and group chats, fetched through Beeper Desktop's `/v1/assets/serve` and cached as small thumbnails. This also fixes the stored participant list of direct chats, which was unreadable (`JsonDataAccess::saveToBuffer` appends to the string it is given).
- **Dark theme**: the original has a dark mode too since 1.2.3; this one predates it and works differently. It switches right away without restarting the app (**Settings > Dark Mode**, on by default), and all pages (chats, chat list, contacts, emoji, search, Active Frame) take their colors from one WhatsApp-like palette. The light theme uses the original's colors.
- **Voice messages without BerryCore**: received voice notes play inside the chat (Ogg/Opus decoded on the phone), and you can record and send your own (with an empty text field the send button becomes a microphone). Recording uses the phone's voice-recording audio path with the driver's mmap mode off, which removes the crackling it otherwise produces.
- **WhatsApp voice calls** (experimental, see [below](#-whatsapp-calls-experimental)): incoming and outgoing 1:1 calls with a call screen, through a WaCalls server.
- **Active Frame**: the minimized app shows each account with its unread messages (archived chats left out, muted ones counted and said).
- **Safer settings**: the sync cursor and the new-message flags, which the app and its service both rewrite all the time, moved out of the main settings file; sharing it could empty it (server, token and accounts lost).
- **`package.ps1`**: builds the app and its service from the command line into one `.bar`, and can install it on a rooted phone over SSH (`-Install`; set `$PhoneIp`/`$RootKey` in `package.config.ps1`, see `package.config.example.ps1`). It needs the BlackBerry 10 Native SDK 10.3 in `C:\bbndk` and Git for Windows.

## 📞 WhatsApp calls (experimental)

Berry Bridge can make and receive **WhatsApp calls** on the BlackBerry. Beeper doesn't carry calls, so they go through [WaCalls](https://github.com/JotaDev66/WaCalls), a server that implements WhatsApp's VoIP stack (signaling, MLow codec, SRTP relays) on top of whatsmeow, plus a small **Berry Bridge gateway** added to it in [damianolampisti00/WaCalls](https://github.com/damianolampisti00/WaCalls#berry-bridge-gateway-blackberry-10) (`cmd/server/bbgateway.go`). The in-app call screen carries 16 kHz audio (Opus, about 24 kbit/s each way, or raw PCM with an older gateway) and simple JSON over one TLS WebSocket. Calls can also run in the phone's native Phone app through the SIP/RTP relay: voice with Opus (PCMU fallback) and 1:1 H.264 video both ways, with WaCalls started with `-sip-addr` and (for video) `-wa-video`.

```
WhatsApp <-> WaCalls (linked device of your account) <-> gateway (wss, token) <-> Berry Bridge service
                                                                                   |-> in-app call screen (voice)
                                                                                   +-> SIP/RTP relay <-> native Phone app (voice + H.264 video)
```

- **Incoming calls**: like a phone call, the screen turns on and a system dialog with *Rispondi* / *Rifiuta* shows up, also on the lock screen (the password prompt appears with it if the phone is locked with one). When the native call engine is registered, the same call also rings in the native Phone app; answering either one claims the call and stops the other. Sound and vibration follow the active profile and Berry Bridge's notification settings (Settings > Notifications). The call screen comes to the front, plus a Hub notification (and a missed-call one if you don't answer). The call also rings on your main phone, as on any linked device.
- **Outgoing calls**: the ☎ button places a voice call in the native Phone app when the root dialhelper is reachable, falling back to the in-app screen otherwise; the camera button places a video call in the native Phone app. Both use the number from Beeper's participant list. Video requires WaCalls `-wa-video` and the updated root dialhelper.
- **In a call**: timer, mute, speaker/earpiece, hang up in the in-app screen; calls in the native Phone app use its own call screen and audio path. The call lives in the service, so hiding the screen or closing the app doesn't end it. A quiet "Chiamata WhatsApp in corso" notification in the Hub brings the call screen back.
- **Audio (in-app screen)**: 16 kHz mono, 20 ms blocks, the system's voice audio path. Playback goes through a jitter buffer that sizes itself to how late packets arrive (80-480 ms) and refills after every pause (no packets come while the peer is silent), so network jitter doesn't chop the speech; the gateway buffers the phone's audio the same way. Each mic block carries the speaker position it was recorded against (counted, not timed), so the gateway can cancel the phone's own earpiece/speaker echo; it also cancels the user's voice coming back from a hands-free peer, and levels both voices. About 10% CPU on a Q10. The certificate of the gateway is verified.
- **Native Phone app**: the SIP/RTP relay carries the engine's calls end to end, with Opus offered first (PCMU fallback) and H.264 video both ways: the camera's access units are repacketized for WhatsApp, and the peer's video is depacketized, CVO-corrected and handed to the engine whole. On video calls the gateway splits WhatsApp's 120 ms two-frame MLow blocks and keeps a 140 ms pre-buffer. While the phone rings, AnyFirewall/STUN probes every 50 ms set the engine's remote-data flag as soon as Answer is tapped, so the call connects quickly even over LTE.

Setup:

1. Run [the WaCalls fork](https://github.com/damianolampisti00/WaCalls) with the gateway on its own listener and a random token (16+ characters) in a file, e.g. `wacalls -addr 127.0.0.1:8097 -bb-addr 127.0.0.1:8098 -bb-token-file bb_token -sip-addr 127.0.0.1:5070 -wa-video`, and pair it with your WhatsApp (QR, *Linked devices*). Keep the WaCalls API itself on localhost: it has no authentication. `-sip-addr` enables the SIP gateway the native Phone app uses (without it calls there are refused); Opus is offered by default (`-sip-opus` with `-sip-delayed-offer`, PCMU as fallback); `-wa-video` enables H.264 video both ways.
2. Expose only the gateway, over HTTPS (for example a Cloudflare tunnel to `localhost:8098`; HTTP/2 transport is the safer choice for a long-lived audio stream).
3. In Berry Bridge **Settings** (menu > Settings): *Calls server* `wss://your-host/ws` and *Calls token*. The status line below them shows the link state.

Not done yet: group calls. The Berry Bridge in-app call screen is audio-only; 1:1 video uses the native Phone app and the SIP/RTP path, H.264 both ways.

For development, `package.ps1 -DevTools` adds a call-audio test harness (driven by a file in the shared folder) and debug logs in the app's data folder. Never use it for builds you give to others: the shared folder is reachable by other apps.

Credits: Berry Bridge by Adem Zengin (MIT, see `LICENSE`); the Ogg/Opus encoder and decoder and the mbedTLS network backend are adapted from BBport (GPL-3.0-only, see `BerryBridgeUI/src/net/COPYING.GPL-3.0`); mbed TLS 2.28.9 is Apache-2.0 (`third_party/mbedtls/LICENSE`); libopus is BSD-licensed (`third_party/opus/COPYING`); calls rely on WaCalls by JotaDev66 (MIT) and whatsmeow.

## 📱 Screenshots

<table>
  <tr>
    <td width="33%"><img src="https://github.com/user-attachments/assets/de06ed7f-2a49-4c55-9be2-2857585999b5" alt="1" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/90861b61-60aa-4c00-ac91-abf4db7fcf65" alt="2" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/f1dcec03-d671-4ae6-bed4-1aa1478c5a81" alt="3" /></td>
  </tr>
  <tr>
    <td width="33%"><img src="https://github.com/user-attachments/assets/3afe5d47-6f1b-4d3e-a8a6-f7313baf9f7c" alt="4" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/9d7dd451-42d9-4813-abcd-950f1fa4139e" alt="5" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/4f1a4517-ce6f-44dc-b9eb-8080bcf7299d" alt="6" /></td>
  </tr>
  <tr>
    <td width="33%"><img src="https://github.com/user-attachments/assets/f2a3cc8e-b2b4-442a-b2d5-d061da0928b8" alt="7" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/abe9740b-2166-49e8-8b6c-43489a204a8d" alt="8" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/acc44d0d-001c-402d-af72-c3d70d87169b" alt="9" /></td>
  </tr>
  <tr>
    <td width="33%"><img src="https://github.com/user-attachments/assets/ee81c9e5-6a45-43f8-9335-7f564153f2fb" alt="9.5" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/eefeb098-5b4c-4098-9b5e-29e10ed057a2" alt="10" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/8fef73ee-a429-4c3c-8367-39864bb9cec0" alt="11" /></td>
  </tr>
  <tr>
    <td width="33%"><img src="https://github.com/user-attachments/assets/a0524f64-46cf-4ddc-92f8-4ff4f50f3a71" alt="12" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/03c90992-4b75-4bc8-8e08-be76abcc063e" alt="13" /></td>
    <td width="33%"><img src="https://github.com/user-attachments/assets/3be43ced-ba77-41a5-bce4-e6bacefd403d" alt="14" /></td>
  </tr>
</table>
To use this application, you must first set up the Beeper Desktop application on a PC (for local network use) or on a cloud server (for remote access anywhere). Key features of the application and detailed setup instructions are provided below.

## ✨ Key Features

- **Multi-Account Tabbed UI:** Every linked account gets its own dedicated tab.
- **Custom Color Palettes:** Easily distinguish between different accounts with customizable theme palettes.
- **Battery-Efficient Push Service:** Native background connectivity designed for minimal battery consumption.
- **Rich Messaging:** Send and receive text messages, media attachments, and files.
- **BlackBerry Hub Notifications:** Receive native system notifications directly in the BB10 Hub.
- **Chat Management:** Mute, pin, edit, delete, react to, and reply to specific messages.
- **Message Status Indicators:** Clear indicators for **Pending**, **Sent**, and **Seen** states.
- **Secure Communication:** Encrypted HTTPS/TLS connections between your BB10 device and the Beeper Desktop API.

## 🏠 Option 1: Windows PC Setup (LAN / Home & Office Use)

Follow these steps or watch the setup guide on YouTube to connect your BlackBerry 10 device to Beeper Desktop running on your local Windows PC.

[![## 📸 Video Tutorial](https://img.youtube.com/vi/bYO2Vhnvnmg/0.jpg)]([https://www.youtube.com/watch?v=bYO2Vhnvnmg](https://www.youtube.com/watch?v=bYO2Vhnvnmg))

### 1. Configure Beeper Desktop
1. Download and install **Beeper Desktop** on your Windows PC.
2. Sign in / Sign up and link your messaging accounts.
3. Go to **Settings > Integrations > Advanced**.
4. Enable **Remote Access** and restart the app.
5. Go to **Settings > Integrations > Approved Connections**.
6. Click the **`+`** icon to add a new connection.
7. Enter a name for your access token.
8. Set **Expires In** to **NEVER**.
9. Set **Allow sensitive actions** to **YES**.
10. Click **Create**. Copy the generated token and save it to a text file. *(This will be your Access Token)*.

### 2. Find Your PC's Local IP Address
1. Open Command Prompt (`cmd`) on your PC and run `ipconfig`.
2. Find your **IPv4 Address** (e.g., `192.168.1.50`).
3. Your **Server URL** will use port `23373` in the following format:
   ```text
   http://YOUR_LOCAL_IPV4:23373
   ```
   **Security:** Beeper's local API uses plain HTTP here. The access token and message traffic are not encrypted on the LAN. Use this only on a trusted, private network; do not forward port `23373` to the internet. For remote access, use the HTTPS cloud setup below.

### 3. Configure Windows Firewall
1. Open the Windows Start menu, search for **Windows Security**, and launch it.
2. Go to **Firewall & network protection** > click **Advanced settings**.
3. Select **Inbound Rules** in the left menu, then click **New Rule...** in the right menu.
4. Select **Port** and click **Next**.
5. Choose **TCP** and enter `23373` under **Specific local ports**, then click **Next**.
6. Select **Allow the connection** and click **Next**.
7. Uncheck **Public** (keep *Domain* and *Private* checked) and click **Next**.
8. Name the rule (e.g., `Beeper Remote Access`) and click **Finish**.

### 4. Setup BlackBerry 10 Device
1. Send the **Server URL** and **Access Token** to your BB10 device (via email).
2. Launch **Berry Bridge** and navigate to the **Settings** page.
3. Copy and paste the **Server URL** and **Access Token**.
4. Tap **List Accounts** to connect and sync!

---

## ☁️ Option 2: Free Cloud Server Setup (Global Access)

To access your messages outside your local network without keeping a home PC constantly running, you can host Beeper Desktop on a free cloud server (e.g., Oracle Cloud Always Free).

### 1. Create an Oracle Cloud VM
1. Sign up for a free **Oracle Cloud** account.
2. Create a Virtual Machine (VM) eligible under the **Always Free** tier (e.g., Ubuntu ARM64).
3. **Save your SSH private key** during instance creation.
4. Once created, note down your server's **Public IP Address**.

### 2. Open Ports in Oracle Cloud VCN Security Rules
1. Go to **Instances** > Click on your instance name.
2. Under **Instance details**, click on your **Virtual Cloud Network (VCN)**.
3. Select **Subnets** > Click on your subnet.
4. Click on the **Default Security List**.
5. Under **Ingress Rules**, click **Add Ingress Rules**.
6. Add ingress rules for TCP ports **`80`** and **`443`** (HTTP is used for Let's Encrypt certificate issuance/renewal and redirects to HTTPS). Do not expose RDP (`3389`) or Beeper's API port (`23373`) publicly. Restrict SSH (`22`) to your current public IP address where the cloud firewall supports it.

### 3. Server Configuration over SSH
Connect to your server via SSH (e.g., using Terminal or PuTTY):

```bash
ssh -i /path/to/your_private_key.key ubuntu@YOUR_PUBLIC_IP
```

Install XFCE and Firefox for the remote desktop session:
```bash
sudo apt update
sudo apt install xfce4 xfce4-goodies firefox -y
echo "xfce4-session" > ~/.xsession
```

Keep administrative access on SSH. If a graphical session is needed, use an SSH tunnel to a separately configured, localhost-bound RDP service; do not add a public `3389` firewall rule. Open HTTP/HTTPS for the site (and SSH from trusted addresses):
```bash
sudo iptables -I INPUT -p tcp -m multiport --dports 80,443 -j ACCEPT
sudo netfilter-persistent save
```

### 4. Dynamic DNS & SSL Certificate Setup
1. Register a free domain/hostname (e.g., via [DuckDNS](https://www.duckdns.org/)) and point it to your server's **Public IP Address**.
2. Install Nginx and Certbot:
   ```bash
   sudo apt install nano nginx certbot python3-certbot-nginx -y
   ```
3. Generate an SSL certificate:
   ```bash
   sudo certbot certonly --nginx -d YOUR_DOMAIN.duckdns.org --key-type rsa
   ```

### 5. Configure Nginx Reverse Proxy
Create the Nginx configuration file:
```bash
sudo nano /etc/nginx/sites-available/beeper
```

Paste the following configuration (replace `YOUR_DOMAIN.duckdns.org` with your actual domain):

**BB10 compatibility:** BerryBridge's HTTPS API on the Q10 uses vendored mbedTLS 2.28.9; a standalone probe verified a TLS 1.2 connection. Qt's `QSslSocket` push connection succeeds with the current host, but that host also accepts TLS 1.0, so this does not establish which protocol was negotiated. A `QSslSocket` probe constrained to the host-supported `ECDHE-ECDSA-AES128-GCM-SHA256` TLS 1.2 cipher failed, while direct OpenSSL TLS 1.2 succeeded. Therefore, do not claim push or call compatibility with a TLS-1.2-only endpoint until those paths are migrated or verified. Keep the public endpoint on TLS 1.2; do not weaken its TLS settings.

```nginx
server {
    listen 80;
    server_name YOUR_DOMAIN.duckdns.org;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl;
    server_name YOUR_DOMAIN.duckdns.org;

    client_max_body_size 500M;

    ssl_certificate /etc/letsencrypt/live/YOUR_DOMAIN.duckdns.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/YOUR_DOMAIN.duckdns.org/privkey.pem;

    # Keep the public endpoint on TLS 1.2. BerryBridge's HTTP API transport
    # uses mbed TLS; push and call WebSockets remain unverified (see above).
    ssl_protocols TLSv1.2;
    ssl_prefer_server_ciphers on;
    ssl_ciphers 'ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384';
    access_log /var/log/nginx/beeper_access.log ssl_verbose;
    error_log /var/log/nginx/beeper_error.log info;

    location / {
        proxy_pass http://127.0.0.1:23373;
        proxy_http_version 1.1;

        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";

        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;

        proxy_read_timeout 86400s;
        proxy_send_timeout 86400s;
    }
}
```

Enable the site configuration and restart Nginx:
```bash
sudo ln -s /etc/nginx/sites-available/beeper /etc/nginx/sites-enabled/
sudo rm -f /etc/nginx/sites-enabled/default
sudo systemctl restart nginx
```

### 6. Connect to the desktop privately & Install Beeper Desktop
1. Keep RDP bound to `127.0.0.1` on the server and allow it only through an SSH tunnel. Do not expose port `3389` on the public IP. For example, from a trusted computer run `ssh -i /path/to/key -L 13389:127.0.0.1:3389 ubuntu@YOUR_PUBLIC_IP`, then connect Remote Desktop to `localhost:13389`.
2. Log in using the server account.
3. Open **Firefox** inside the remote desktop session.
4. Download Beeper Desktop:
   [https://api.beeper.com/desktop/download/linux/arm64/stable/com.automattic.beeper.desktop](https://api.beeper.com/desktop/download/linux/arm64/stable/com.automattic.beeper.desktop)
5. Open the **Downloads** folder, right-click the `.AppImage` file > **Properties** > **Permissions** > Check **Allow this file to run as a program**.
6. Double-click the file to launch **Beeper Desktop**.
7. Sign in, link your messaging accounts.
8. Generate your **Access Token** under **Settings > Integrations > Approved Connections** (Set *Expires In* to **NEVER** and *Allow sensitive actions* to **YES**). Save this token securely.

### 7. Connect Berry Bridge on BlackBerry 10
1. Open **Berry Bridge** on your BB10 device and go to **Settings**. The HTTP API path uses mbed TLS 2.28.9 and TLS 1.2. Push notifications and WhatsApp call signaling still use Qt TLS and are not expected to work against this TLS 1.2-only endpoint.
2. Enter your domain name as the **Server URL** (e.g., `https://YOUR_DOMAIN.duckdns.org`).
3. Paste your generated **Access Token**.
4. Tap **List Accounts** and enjoy global access!
