# MIDI-BLE-RT GUI

A GUI do MIDI-BLE-RT está em desenvolvimento para a versão 1.0. O objetivo é oferecer uma interface simples para músicos, separando claramente três estados diferentes:

- dispositivo conhecido pelo BlueZ;
- instrumento descoberto por scan BLE-MIDI;
- instrumento importado/configurado no MIDI-BLE-RT.

O daemon já pode ser utilizado pela linha de comando. A GUI apenas organiza o fluxo de descoberta, importação, pareamento e conexão.

## Regras funcionais da GUI

1. A lista principal deve mostrar apenas dispositivos/instrumentos importados no MIDI-BLE-RT.

2. O botão **Adicionar instrumento** deve abrir o diálogo de pareamento/importação.

3. No diálogo, o usuário pode parear um instrumento novo ou importar um instrumento já pareado no BlueZ. Os botões **Importar** e **Parear** devem aparecer junto de cada dispositivo descoberto e devem ser habilitados conforme cada caso:
   - dispositivo já pareado no BlueZ: **Importar** habilitado e **Parear** desabilitado;
   - dispositivo ainda não pareado no BlueZ: **Parear** habilitado e **Importar** desabilitado.

4. Após clicar em **Importar** ou **Parear**, a GUI deve apresentar mensagem de sucesso ou falha. Toda falha também deve ser registrada no log da GUI.

5. Ao importar, a GUI deve criar o arquivo de configuração do instrumento na pasta usada pelo daemon:

   ```text
   ~/.config/midi-ble-rt/devices.d/
