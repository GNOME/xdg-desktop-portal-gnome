# Demo

Build and run [ashpd-demo](https://github.com/bilelmoussaoui/ashph) to try out local changes:
```shell
./build-and-run.sh ashpd-demo
```
(This temporarily overrides `xdg-desktop-portal-gnome.service`.)

Showcase the monitor selection widget for different display configurations:
```shell
./mock-build-and-run.sh ashpd-demo
```
And follow output with:
```shell
./follow-xdpg-log.sh
```
