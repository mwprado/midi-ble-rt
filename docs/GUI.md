# MIDI-BLE-RT GUI

A GUI do MIDI-BLE-RT está em desenvolvimento para a versão 1.0. O objetivo é oferecer uma interface simples para músicos, separando claramente três estados diferentes:

- dispositivo conhecido pelo BlueZ;
- instrumento descoberto por scan BLE-MIDI;
- instrumento importado/configurado no MIDI-BLE-RT.

O daemon já pode ser utilizado pela linha de comando. A GUI apenas organiza o fluxo de descoberta, importação, pareamento e conexão.

## Lista principal

A lista principal da janela mostra somente instrumentos importados/catalogados no MIDI-BLE-RT.

Fonte da lista:

```text
~/.config/midi-ble-rt/devices.d/*.ini
