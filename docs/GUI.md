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
   ```

6. A lista principal da janela deve listar os dispositivos/instrumentos configurados a partir dos arquivos de configuração existentes.

7. Um dispositivo já importado pode ter sido despareado, esquecido ou perdido no BlueZ. Nesse caso, ele deve continuar aparecendo na lista principal, porque ainda existe configuração local, mas deve constar como **Indisponível** ou **Pareie novamente**.

## Lista principal

A lista principal da janela mostra somente instrumentos importados/catalogados no MIDI-BLE-RT.

Fonte da lista:

```text
~/.config/midi-ble-rt/devices.d/*.ini
```

Um dispositivo que aparece no scan do BlueZ não deve aparecer automaticamente na janela principal. Ele só passa a aparecer depois de ser importado ou pareado pela GUI.

## Adicionar instrumento

O botão **Adicionar instrumento** abre o diálogo de descoberta/importação.

Esse diálogo lista instrumentos BLE-MIDI conhecidos ou descobertos pelo BlueZ. Para cada linha, a GUI mostra ações conforme o estado do dispositivo:

```text
Dispositivo já pareado no BlueZ
  → botão Importar habilitado
  → botão Parear desabilitado

Dispositivo ainda não pareado no BlueZ
  → botão Importar desabilitado
  → botão Parear habilitado

Dispositivo já importado no MIDI-BLE-RT
  → não deve ser tratado como novo instrumento
```

## Importar

A ação **Importar** é usada quando o instrumento já está pareado no BlueZ, mas ainda não tem configuração no MIDI-BLE-RT.

Fluxo esperado:

```text
Importar
  → marcar Trusted=true quando aplicável
  → criar arquivo de configuração em devices.d/
  → solicitar daemon-recheck
  → atualizar a lista principal
```

Após sucesso, o instrumento deve aparecer na lista principal.

Em caso de falha, a GUI deve apresentar mensagem clara ao usuário e registrar o erro no log da GUI.

## Parear

A ação **Parear** é usada quando o instrumento ainda não está pareado no BlueZ.

Fluxo esperado:

```text
Parear
  → executar Pair no BlueZ
  → marcar Trusted=true
  → criar arquivo de configuração em devices.d/
  → solicitar daemon-recheck
  → atualizar a lista principal
```

Após sucesso, o instrumento deve aparecer na lista principal.

Em caso de falha, a GUI deve apresentar mensagem clara ao usuário e registrar o erro no log da GUI.

## Instrumento importado indisponível

Um instrumento pode estar importado/configurado no MIDI-BLE-RT, mas ter sido despareado, esquecido ou perdido no BlueZ.

Nesse caso, ele deve continuar aparecendo na lista principal, porque ainda existe configuração local, mas com estado visual de indisponível.

Comportamento esperado:

```text
Arquivo .ini existe
BlueZ não possui pairing válido
  → listar na janela principal
  → estado: Indisponível
  → ação recomendada: Parear novamente
```

A GUI não deve apagar automaticamente a configuração local só porque o BlueZ não possui mais o pareamento.

## Regra central

```text
Janela principal
  = catálogo local do MIDI-BLE-RT

Diálogo Adicionar instrumento
  = descoberta BlueZ / BLE-MIDI

Importar
  = BlueZ já pareado → criar configuração MIDI-BLE-RT

Parear
  = BlueZ ainda não pareado → pair + trust + criar configuração MIDI-BLE-RT
```
