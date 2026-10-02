# Xorg source reference

`xorg-server-21.1.12/` is the unmodified upstream Xorg server 21.1.12
source tree, extracted from:

https://xorg.freedesktop.org/releases/individual/xserver/xorg-server-21.1.12.tar.xz

The archive may be retained locally in `source-packages/`. Its SHA-256 is:

`1e016e2be1b5ccdd65eac3ea08e54bd13ce8f4f6c3fb32ad6fdac4e71729a90f`

The development VM used Ubuntu server package `2:21.1.12-1ubuntu1.6` when
this source was inspected. The upstream source matches its base version but
does not include Ubuntu's packaging patches. Use it as an API reference;
review distro patches before building or deploying an Xorg-integrated XEH
binary.

The extracted source and archive are local reference material and are ignored
by Git. Clone the published XEH repository without them; fetch the upstream
Xorg source or install an SDK matching your server when integration work needs
those headers. Neither the upstream tree nor its license is part of XEH's
MIT-licensed source distribution.
