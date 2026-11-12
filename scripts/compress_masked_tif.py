import rasterio
from rasterio.windows import Window
import numpy as np

# Caminhos dos arquivos
HIGH_RES_TIF = "C:\\Users\\pedro\\OneDrive\\Área de Trabalho\\bruto_2012_z21.tif"     
MASK_TIF     = "C:\\Users\\pedro\\OneDrive\\Área de Trabalho\\bruto_2012_mascara.tiff" 
OUTPUT_TIF   = "C:\\Users\\pedro\\OneDrive\\Área de Trabalho\\satelite_otimizado.tif"        

# Tolerância para considerar um pixel como "preto" (0 = apenas preto absoluto 0,0,0;
# <= 3 ajuda caso o editor salve com leve artefato quase-preto)
BLACK_THRESHOLD = 3

with rasterio.open(HIGH_RES_TIF) as src, rasterio.open(MASK_TIF) as mask_src:
    print(f"Original : {src.width}x{src.height} ({src.count} canais)")
    print(f"Máscara  : {mask_src.width}x{mask_src.height} ({mask_src.count} canais)")

    # Força saída com 3 canais (RGB) sem canal Alpha extra para economizar espaço
    out_channels = min(3, src.count)

    profile = src.profile.copy()
    profile.pop('jpeg_quality', None)
    profile.pop('quality', None)
    profile.pop('predictor', None)

    profile.update(
        count=out_channels,
        photometric="ycbcr",   # Padrão nativo do arquivo original
        compress="jpeg",       # Compressão de alta eficiência
        jpeg_quality=75,       # 75 = reduz de 104 MB para ~75-80 MB mantendo ótima fidelidade
        tiled=True,
        blockxsize=256,        # Idêntico ao original (256x256)
        blockysize=256,
        BIGTIFF="NO"           # OBRIGATÓRIO: grava como Classic TIFF (0x2a) para abrir nativo no Windows
    )

    # Janelas de processamento de 1024x1024 na imagem grande (rápido e leve na RAM)
    STEP = 1024
    total_blocks = ((src.height + STEP - 1) // STEP) * ((src.width + STEP - 1) // STEP)
    done_blocks = 0

    with rasterio.open(OUTPUT_TIF, "w", **profile) as dst:
        for row_off in range(0, src.height, STEP):
            win_h = min(STEP, src.height - row_off)

            # Mapeia as linhas de 62635x47722 para 15659x11930
            rows = np.arange(row_off, row_off + win_h, dtype=np.int64)
            mask_rows = np.clip((rows * mask_src.height) // src.height, 0, mask_src.height - 1)
            m_row_start = int(mask_rows[0])
            m_row_end   = int(mask_rows[-1]) + 1

            for col_off in range(0, src.width, STEP):
                win_w = min(STEP, src.width - col_off)
                window = Window(col_off, row_off, win_w, win_h)

                # Mapeia as colunas de 62635x47722 para 15659x11930
                cols = np.arange(col_off, col_off + win_w, dtype=np.int64)
                mask_cols = np.clip((cols * mask_src.width) // src.width, 0, mask_src.width - 1)
                m_col_start = int(mask_cols[0])
                m_col_end   = int(mask_cols[-1]) + 1

                # Lê apenas o sub-bloco correspondente na máscara de 15659x11930
                mask_win = Window(
                    m_col_start,
                    m_row_start,
                    m_col_end - m_col_start,
                    m_row_end - m_row_start
                )
                m_data = mask_src.read(window=mask_win)

                # Identifica onde a máscara NÃO é preta (e NÃO é transparente, se tiver Alpha)
                if mask_src.count >= 3:
                    valid_small = (
                        (m_data[0] > BLACK_THRESHOLD) |
                        (m_data[1] > BLACK_THRESHOLD) |
                        (m_data[2] > BLACK_THRESHOLD)
                    )
                    if mask_src.count >= 4:
                        valid_small &= (m_data[3] > 0)
                else:
                    valid_small = (m_data[0] > BLACK_THRESHOLD)

                # Se o bloco inteiro for preto na máscara, escreve zeros direto sem nem ler o bloco de alta resolução!
                if not np.any(valid_small):
                    zero_block = np.zeros((out_channels, win_h, win_w), dtype=src.dtypes[0])
                    dst.write(zero_block, window=window)
                else:
                    # Lê o bloco de alta resolução apenas onde há área útil
                    data = src.read(list(range(1, out_channels + 1)), window=window)

                    # Expande a máscara para a resolução exata do bloco (mapeamento 1:4 exato)
                    valid_full = valid_small[
                        np.ix_(mask_rows - m_row_start, mask_cols - m_col_start)
                    ]
                    data[:, ~valid_full] = 0
                    dst.write(data, window=window)

                done_blocks += 1
                if done_blocks % 200 == 0 or done_blocks == total_blocks:
                    print(f"Progresso: {done_blocks}/{total_blocks} blocos ({100.0 * done_blocks / total_blocks:.1f}%)")

print(f"Concluído! Arquivo salvo em: {OUTPUT_TIF}")