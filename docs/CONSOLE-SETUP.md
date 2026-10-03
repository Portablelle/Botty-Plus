# Open Botty+ through the PS5 User's Guide

The User's Guide can open Botty+ when a dedicated DNS resolver redirects the
Guide hostname to your portal. First deploy the HTTPS site and the
[DNS/Guide redirect services](../deployment/README.md#optional-users-guide-dns),
or obtain the resolver's IP address and expected portal URL from your host
administrator. Hosting the static site alone does not enable this entry point.

`DNS_IP` below means the reachable IPv4 address of that configured resolver.
Enter its numeric address on the console, not `DNS_IP`, a URL, or the sample
`203.0.113.10` from the configuration files. The repository does not provide a
public DNS service. A VPS resolver must allow your home connection's public IP
through its DNS firewall rules.

### 1. Change DNS on the PS5

1. Record your current DNS settings so you can restore them later.
2. From the home screen, open **Settings → Network → Settings → Set Up Internet Connection**.
3. Highlight your connected Wi-Fi network or wired LAN connection, press
   **Options**, then select **Advanced Settings**.
4. Change **DNS Settings** to **Manual** and enter:

   | Setting | Value |
   | --- | --- |
   | Primary DNS | `DNS_IP` |
   | Secondary DNS | The same `DNS_IP`, or a second resolver with identical Guide redirects |

5. Keep your existing IP address, DHCP, MTU and proxy settings. Select **OK** to
   save and reconnect. Apply this to the connection you actually use; switching
   between Wi-Fi and Ethernet may use a different saved configuration.

Do not use Google, Cloudflare or your normal router DNS as the secondary for
this setup: it can resolve the original Guide instead of the Botty+ redirect.
The bundled resolver also blocks PlayStation service domains, so PSN sign-in or
the console's Internet test may fail even when the portal is reachable.

### 2. Open the Guide and launch Botty+

1. After a cold restart, open **Settings → User's Guide, Health and Safety, and
   Other Information → User's Guide**. On versions with the **Guide & Tips**
   menu label, open that section, then **Guide and Tips → User's Guide**.
2. The Guide should redirect to your configured **HTTPS Botty+ portal**. Check
   that the destination is the portal URL supplied by your host administrator.
   The sample redirect uses a self-signed certificate for the Guide hostname;
   a warning may appear at that hop. Continue only for your expected configured
   redirect if the browser offers that option. The destination portal needs a
   trusted certificate. If the browser refuses the hop, this route cannot be
   used on that setup; see the troubleshooting table below.
3. Select **LAUNCH** once and leave the page open until it shows **READY**.
4. Press **PS**, return to the home screen and open **Botty+**. Allow time for
   the icon to appear on first installation. Repeat the Guide → **LAUNCH**
   sequence after each cold boot, before opening the native app.

The DNS redirect route still needs hardware acceptance on each intended setup;
tested native navigation does not validate the Guide's certificate behavior.
For the console menu references, see PlayStation's
[network settings](https://www.playstation.com/en-us/support/connectivity/internet-connect-playstation/)
and [User's Guide instructions](https://www.playstation.com/en-us/support/hardware/ps5-console-users-guide/).

### If the Guide does not open the portal

| Symptom | What to check |
| --- | --- |
| The original PlayStation guide appears | Recheck DNS on the active connection and both DNS fields. Close the Guide and reconnect after saving. |
| DNS error or timeout | Check that the resolver is running and reachable, and that its firewall permits your current client address on UDP and TCP 53. |
| Certificate warning or refusal | Verify the Guide redirect configuration and the target portal's certificate. A refused Guide hop requires another working browser entry point. |
| Redirect works but the portal will not load | Check the redirect URL, portal DNS record, HTTPS certificate and web server. |
| LAUNCH is disabled | Check the firmware support message on the portal; DNS settings do not add firmware compatibility. |

To undo the change, return to the same connection's **Advanced Settings** and
restore your previous DNS configuration (**Automatic** if that was the original
setting). This also removes the Guide redirect for that connection.

