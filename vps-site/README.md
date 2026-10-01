# PS5 Relapse Exploit
Supported firmware: 7.00 through 13.60.

## Usage
Press **LAUNCH** once and keep the page open. The portal runs the jailbreak,
verifies/installs Botty Native, loads Kstuff and ShadowMountPlus, starts FTP on
2121, and prepares Transmission and the Botty service. Installed package files
are verified and reused; running services are preserved. FTP is an in-memory
payload, so it is loaded again after a console restart, with no disk installation.

After completion, press PS and open **Botty Native Preview**. Home screen
discovery is asynchronous; successful payload delivery does not prove registration.
On failure, the session log opens automatically; restart before another attempt.
A different or damaged existing native title is preserved and requires a separate
update. Native files are staged outside the library and published as one directory.
The one-button installation flow still requires real-console validation.

The default payloads are stored in `payloads/` after a successful run, the ELF loader listens on port `9021`.

## Stability notes
Webkit may need several attempts, reload the page if the browser stalls. The kernel exploit may hang or panic the console, so reboot before trying again if that happens.

## Exploit chain
Browser stage uses JSC info leaks and a structured clone object pool mismatch to corrupt a typedarray. The kernel stage combines a address leak with an `aio_multi_wait` uaf race to establish kernel r/w.

## Credits
- Sonic_Iso: Kernel Exploit
- Jordy: Webkit Exploit and Kernel Bug
- ntfargo: Exploit Dev
- ufm42: Exploit Dev
- Dr. Yenyen: Testing

Other helps:
- TheFlow, SlidyBat, Flatz, cow, nhk, bollarz, Sleirsgoevy, EchoStretch, EarthOnion.
 
## Disclaimer
This project is intended for **educational and security research purposes only**. It does not endorse piracy, unauthorized access, or misuse of commercial devices. Use it only on devices you own or are authorized to test, and comply with applicable laws and regulations.

The software is provided as-is, without warranty. You assume the risks of using it, including system instability, data loss, and account bans. The maintainers accept no liability for resulting damage.