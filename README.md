# OpenVR Wayland Desktop

A simple OpenVR overlay to use your monitor(s) in VR. Finally, you don't have to worry about Big Valve taking their time with fixing the desktop overlay.

<img width="600" height="337" alt="overlay-demo" src="https://github.com/user-attachments/assets/4f5a866d-948a-44d9-91a4-d6804de36e0f" />

## Features

- **Automatic start with SteamVR**: You can install the overlay as an OpenVR dashboard application, and it will automatically start with SteamVR.
- **Multiple monitors**: You can add multiple monitors each as different overlays.
- **Tray Icon**: You can manage the overlay settings through a tray icon.
- **DMA-BUF support**: This can be enabled per-monitor, but it is off by default. It usually works, but it will definitely not work for cross-GPU setups. Enabling this can help with improving performance when it works.
- **It's a dashboard overlay!** - Yay!!!


## Caveats

- The SteamVR keyboard is fairly limited, with no way to use CTRL, function keys, super key, etc. The *old* SteamVR keyboard (when the new dashboard fails to load) works half of the time purely by chance.
- There is a evdev tracker to allow "interrupting" the pointer mouse when the real mouse is used, but you will have to configure udev rules or permissions.
- Some Wayland compositors will have bugs. I am too lazy to figure out why. KDE Plasma (kwin) and GNOME (mutter) work.
- It is exclusively a tray app, so make sure you have a tray icon extension installed if you want to use this.
