# Hailwire

Hailwire sends incoming EverQuest tells from Hailstone to a private Discord bot DM. Each message shows the sender, receiving character, game server, and time. If you box, tells from all your enabled characters arrive in the same DM thread with the receiving character on each message.

## Set up Hailwire in game

1. Open **Add-ons** in Hailstone 0.7.2-beta or newer. Under **Install from a link**, enter `fryguy503/Hailwire`, select **Look**, then install Hailwire. For a local build, copy the staged `hailwire` folder into `plugins/` in your Hailstone player-data folder. Keep the `ui/` folder beside the plugin library.
2. Sign in to a character and type `/hailwire`. If needed, load the add-on first with `/plugin hailwire`. If Hailwire asks for a relay address, paste the HTTPS address from your operator, such as `https://relay.example.com`, and save it. Choose **Link Discord**.
3. In the Discord server that hosts Hailwire, choose `/hailwire link` and paste the panel’s pairing code into the `code` field. Discord sends a private test DM. Return to Hailstone once it arrives.
4. Turn on forwarding for the current character, or enable the default for all characters and override it from individual characters. Use **Send test DM** to check delivery at any time.

Forwarding starts with new tells while the receiving character is in game. Use the panel to pause forwarding or disconnect this installation. Run `/hailwire unlink` in the bot’s Discord server to disconnect every installation linked to your Discord account. Reply to tells in Hailstone.

If the command is missing, check that you’re using the Discord server where the relay operator installed the bot. If the test DM fails, check that you allow DMs from members of that server, then try linking again. If Hailstone blocks the plug-in, the in-game **Add-ons** and **Plugins** panels show its status and the server’s add-on policy. [Discord DM delivery help](https://support.discord.com/hc/en-us/articles/360060145013-Why-isn-t-my-DM-going-through)

## Allow Hailwire on a game server

For a Hailstone server with `[addons]` enforcement and `default = deny`, append `fryguy503/hailwire` to the existing `allow` list in the server’s `client_files/hailstone.cfg`. Players should install Hailwire through **Add-ons** using the matching release so Hailstone can identify its source. If `[plugins]` defines a capability ceiling, include `read,commands,events,ui,net` in the allowed set. Players receive the updated server policy on their next login.

## Run a relay

One relay and Discord bot serve everyone you invite to the Discord server. Players link with a code and manage forwarding in game. Keep the bot token on the relay host. You’ll need a Discord server you manage and a host with a public HTTPS address so Discord can deliver signed slash-command requests.

### Create the Discord app

1. Create a **Hailwire** application in the Discord Developer Portal. On **General Information**, copy the **Application ID** and **Public Key**. In **Bot**, create or reset the **Token** and copy it once. Leave the privileged gateway intents disabled.
2. In **Installation**, enable **Guild Install**. Under the guild’s default install settings, select `bot` and `applications.commands`. Keep server permissions empty. Follow the install link and add the app to your Discord server.
3. Enable **Developer Mode** under Discord **User Settings → Advanced**. Right-click your Discord server’s icon and choose **Copy Server ID**. Optionally copy a server invite or a channel link for the in-game setup panel.

[Discord bot setup guide](https://docs.discord.com/developers/quick-start/getting-started)

### Start the relay with Docker Compose

Point a public DNS name at your host and allow inbound TCP ports 80 and 443 for Caddy. From the Hailwire checkout, copy `.env.example` to `.env`. Fill in the four Discord values and set `HAILWIRE_DOMAIN` to your DNS name. `HAILWIRE_GUILD_URL` is the optional invite or channel link. Keep `.env` private.

```sh
docker compose --env-file .env -f deploy/compose.yaml up -d --build
curl --fail https://relay.example.com/healthz
docker compose --env-file .env -f deploy/compose.yaml exec relay node register.mjs
```

Replace `relay.example.com` with your domain. On the app’s **General Information** page, set **Interactions Endpoint URL** to `https://relay.example.com/discord/interactions` and save. The running relay responds to Discord’s verification request. Share the bare HTTPS address and the Discord server with your players. [Discord interaction endpoints](https://docs.discord.com/developers/interactions/overview) · [Caddy reverse proxy](https://caddyserver.com/docs/quick-starts/reverse-proxy)

### Run behind an existing HTTPS proxy

Use Node.js 22 or newer. Copy the example `.env` and set the same Discord values.

```sh
node --env-file=.env relay/start.mjs
node --env-file=.env relay/register.mjs
```

The relay listens on `127.0.0.1:8787` by default. Forward traffic from your HTTPS origin to that port, then set the Discord interaction URL as above. Set `HAILWIRE_DATA_DIR` to a private, persistent directory for linked devices. If the proxy overwrites `X-Real-IP` with the verified client address, set `HAILWIRE_TRUST_PROXY=1` and keep the Node listener private. Use a single relay process per data directory. The Compose configuration persists links and certificates in named volumes.

### Data and delivery

The HTTPS relay sees the text and routing information needed to send each DM. It queues tells in memory and keeps credential hashes and Discord routing IDs on disk so links survive restarts. The plug-in saves its device credential in `plugins/hailwire/discord.json` in the player-data folder. Keep that file private. Discord retains the DMs according to its own settings. A relay restart can drop tells that were still queued. The in-game activity count reports relay acceptance, and a test DM confirms Discord delivery.

## Build the add-on

Use CMake 3.22+ and a C++20 compiler. Supply the public Hailstone SDK from the client checkout. Windows uses WinHTTP. Linux and macOS use libcurl. Install Node.js 22+ and Lua 5.4 to run the HTTP and panel tests. Set `HAILWIRE_RELAY_URL` at build time to prefill the operator’s relay for players. Leave it empty for a build that accepts a custom relay address during setup.

```powershell
cmake -S . -B build -A x64 -DHAILSTONE_SDK_DIR=../hailstone-client/sdk -DHAILWIRE_RELAY_URL=https://relay.example.com
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
node --test relay/test/relay.test.mjs
```

Omit `-A x64` with single-architecture generators like Ninja. The staged plug-in is in `stage/hailwire`. For a Hailstone add-on ZIP and manifest, run the client’s packaging tool against that folder.

```powershell
python ../hailstone-client/tools/make_addon.py --name hailwire --kind plugin --version 0.1.0 --min-client 0.7.2-beta --author Fryguy --description 'Forward incoming EverQuest tells to your Discord DMs.' --caps read,commands,events,ui,net --platform windows-x64=stage/hailwire --out release
```

CI builds Windows x64, Linux x64, and macOS arm64 add-on packages. Set the `HAILWIRE_RELAY_URL` Actions variable to include your relay address in those builds. Keep `hailstone-addon.txt` and the ZIPs it references together in the repository root when publishing from `main`. A GitHub release can serve the same files as release assets. Sign the manifest with your Hailstone add-on signing key for clients configured to require signatures.

The released Hailstone ABI exposes formatted chat. Hailwire recognizes standard English player tell formats, including language annotations and qualified sender names. Custom or localized formats may require a parser update.
