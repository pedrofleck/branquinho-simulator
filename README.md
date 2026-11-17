# Branquinho: Rota do Vale

Simulador do ônibus circular da UFRGS ("Branquinho"), desenvolvido como trabalho final da disciplina INF01047 (Computação Gráfica e Visualização I - UFRGS).

## Edição Manual de Terreno (`terreno_editavel.psd`)

Os arquivos `data/terrain/terreno_editavel.psd` e `*.exe` ficam no `.gitignore` por conta do tamanho do PSD. Por isso, **o jogo não executa a ferramenta de PSD automaticamente**: ele carrega diretamente os splatmaps versionados (`data/terrain/splatmap_0.png` e `data/terrain/splatmap_1.png`) e as árvores (`data/terrain/trees.txt`).

Para editar as máscaras de asfalto e materiais manualmente (Affinity Photo / editor com suporte a PSD multicamadas):

1. **Se ainda não tiver o executável ou o PSD gerado localmente**, compile a ferramenta (MSYS2 UCRT64) e gere o PSD a partir dos splatmaps atuais:
   ```powershell
   g++ -O3 -Wall -static tools/terrain_psd_tool.cpp -Icgvis-base-opengl/include -lz -o tools/terrain_psd_tool.exe
   .\tools\terrain_psd_tool.exe --init
   ```
   *(Atenção: `--init` recria `data/terrain/terreno_editavel.psd` a partir dos splatmaps salvos; não rode se tiver edições não aplicadas no PSD.)*

2. **Edite `data/terrain/terreno_editavel.psd`** (`2048x2048`) e exporte sobrescrevendo o próprio `.psd` mantendo todas as camadas de material (`0_Asfalto_Principal` a `3_Grama_Base`) visíveis com `100%` de opacidade.

3. **Aplique manualmente as alterações** nos splatmaps antes de abrir o jogo:
   ```powershell
   .\tools\terrain_psd_tool.exe --apply
   ```
   Isso atualiza `data/terrain/splatmap_0.png` e `data/terrain/splatmap_1.png` sem apagar ou alterar `data/terrain/trees.txt`.

Mais detalhes passo a passo estão em [TUTORIAL_EDICAO_TERRENO.md](TUTORIAL_EDICAO_TERRENO.md) e [TERRAIN_GUIDE.md](TERRAIN_GUIDE.md).
