# Troubleshooting: the web UI

## `http://blsmartflow.local/` does not load

**Why.** mDNS is not resolved by every Windows and Android setup.

**Fix.** Use the device's **IP address** from your router's client list. The device also shows it on
the *Network* page under *Current connection*, and prints `IP_ADDRESS:<ip>` on the serial port on
every successful connect.

=== "Windows"

    Windows 10 and 11 resolve `.local` through the Bonjour service, which is installed by iTunes,
    Adobe apps and some printer drivers — but not by Windows itself. Without it, use the IP.

    `ping blsmartflow.local` is a quick test: *could not find host* means no mDNS.

=== "Android"

    Android's mDNS support is inconsistent between versions and vendors, and some builds only resolve
    `.local` for apps that ask for it explicitly. Chrome on Android often will not. Use the IP.

=== "macOS / iOS / Linux"

    Should work out of the box (Bonjour / Avahi). If it does not, check that your router is not
    blocking multicast between the wireless and wired segments.

!!! tip
    Bookmark the IP address once you have found it, and give the device a **fixed DHCP lease** in your
    router so it never moves.

## The page loads but stays blank / shows old values

Almost always the event stream. Try a hard reload (++ctrl+shift+r++), and check the *Connections*
card — if *Printer data age* is climbing, the device is fine and the printer is the problem.

## The dashboard stops updating in one tab

**Why.** The device accepts at most **4 concurrent event-stream clients**. A fifth connection is
closed immediately.

**Fix.** Close spare tabs. A refused tab is not broken — it falls back to polling every 2 seconds, so
it stays correct, just less lively.

## The captive-portal window misbehaves

The "sign in to network" window on Windows, macOS and Android is a **stripped-down browser**. It
often has no address bar, blocks some JavaScript, and on some builds opens an `EventSource` that
never delivers anything.

**Fix.** Close it and open a **normal browser** at `http://192.168.4.1/`.

The UI already works around the worst of it: in AP mode it polls instead of streaming, it keeps
polling until the stream delivers its first event, and it falls back to polling after 5 seconds of
silence.

## The setup page will not open at all

- Confirm you are actually joined to `BLSmartFlow-xxxx` and not back on your home network — phones
  switch away from networks with no internet.
- Use `http://`, never `https://`. The device has no TLS.
- Some Android builds probe for the portal over HTTPS, which cannot be intercepted. Open
  `http://192.168.4.1/` by hand.

→ [First-time setup](../getting-started/first-setup.md)

## It asks for a password and I do not have one

Basic auth applies to the UI and every API route on your LAN. Two ways back:

1. Reach the device on its **setup network**, where no password is ever asked. (Only available while
   the device cannot reach your WiFi.)
2. Send `{"cmd":"factoryreset"}` over **USB serial**.

→ [Recovery](recovery.md)

## Buttons do nothing / the save bar never appears

Check the log at the bottom of the *System* page and the browser console. If the device is answering
`401`, you have web access enabled and the browser has not sent credentials.

## Curl works but the browser does not

Almost certainly the event stream and not the API. Remember that `curl /api/events` needs the header:

```sh
curl -N -H 'Accept: text/event-stream' http://blsmartflow.local/api/events
```

Without it, `AsyncEventSource` answers 404.

---

Related: [WiFi troubleshooting](wifi.md) ·
[WiFi and the captive portal](../technical/wifi-and-captive-portal.md) ·
[REST API](../technical/rest-api.md)
