#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/generate_terrain_assets.py
===============================================================================
Pipeline offline de geração de terreno, splatmaps multi-material (8 canais),
texturas placeholder nítidas por classe, albedo macro de tonalidade, nivelamento
de vias no heightmap 16-bit e árvores 3D placeholder para o simulador:
"Branquinho: Rota do Vale" (INF01047 - UFRGS).

Regras atendidas:
  1. Usa exclusivamente a imagem de satélite já mascarada (`tools/bruto_2012_mascara.tiff`
     ou `satelite_otimizado.tif`), NUNCA `satelite_2012_low.png` (exclusiva do F11).
  2. Reprojeta o `.tiff` (WGS84 / EPSG:4326) para o espaço métrico exato de
     `data/vale.png` (`1930 x 1703`, SIRGAS 2000 / UTM 22S - EPSG:31982).
  3. Tudo o que está em preto (`RGB = 0,0,0`) na máscara externa e nos recortes
     internos é removido do cenário 3D (`data/terrain/valid_mask.png`), fechando
     apenas micro-falhas (< 9m) causadas por sombras escuras dentro da mata.
  4. Corredores viários refinados por Dijkstra sobre o satélite servem APENAS
     para desobstruir vias ocultas sob árvores escuras, nivelar o perfil
     transversal em `vale_processed.png` e proibir árvores na pista.
  5. Detecta 8 materiais distintos e gera texturas seamless placeholder claras:
       - Canal 0 (splatmap_0.r): mat_0_asphalt_main.png  (Asfalto Principal)
       - Canal 1 (splatmap_0.g): mat_1_asphalt_sec.png   (Asfalto Sec. / Estac.)
       - Canal 2 (splatmap_0.b): mat_2_sidewalk.png      (Calçadas / Lajotas)
       - Canal 3 (splatmap_0.a): mat_3_grass.png         (Grama Aberta)
       - Canal 4 (splatmap_1.r): mat_4_forest.png        (Solo de Mata / Folhas)
       - Canal 5 (splatmap_1.g): mat_5_dirt.png          (Terra Exposta / Barro)
       - Canal 6 (splatmap_1.b): mat_6_water.png         (Água - apenas textura)
       - Canal 7 (splatmap_1.a): mat_7_building.png      (Fundação de Prédios)
===============================================================================
"""

import os
import json
import math
import heapq
import argparse
from collections import deque
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import rasterio
from rasterio.enums import Resampling


# Matriz afim exata mapeando UV normalizado de `vale.png` (UTM 22S) -> UV do `.tiff` (WGS84)
AFFINE_TERRAIN_TO_TIF = np.array([
    [ 9.54578349e-01,  6.50097396e-04],
    [-9.05872281e-04,  9.60028656e-01],
    [ 2.09435199e-02, -5.34539610e-02]
], dtype=np.float64)

# Corredores viários extraídos pixel a pixel diretamente de `bruto_2012_mascara.tiff`
# (no grid métrico 1930x1703), com a estrada coberta por árvores no canto SE.
CAMPUS_ROAD_CORRIDORS = [
    # 1. Anel Viário Principal do Campus do Vale (traçado exato sobre o satélite mascarado)
    {
        "name": "anel_viario_principal",
        "width_m": 9.5,
        "is_main_route": True,
        "force_under_trees": False,
        "waypoints": [
            (281, 1272), (281, 1220), (281, 1170), (284, 1120), (292, 1080),
            (304, 1045), (316, 1016), (330, 990), (350, 956), (365, 928),
            (378, 895), (386, 878), (395, 865), (418, 850),
            (446, 835), (462, 820), (474, 805), (483, 790), (490, 775),
            (496, 760), (518, 715), (532, 690), (569, 640), (588, 615),
            (610, 565), (610, 540), (603, 515), (590, 490), (550, 465),
            (521, 440), (505, 415), (498, 405), (491, 380), (490, 355),
            (490, 330), (498, 305), (511, 280), (528, 255), (545, 230),
            (563, 205), (588, 180), (610, 181), (635, 183), (660, 193),
            (685, 202), (710, 214), (735, 222), (760, 232), (785, 244),
            (810, 262), (827, 280), (846, 320), (861, 360), (872, 400),
            (878, 440), (885, 480), (892, 520), (904, 560), (910, 600),
            (916, 640), (922, 680), (929, 720), (932, 760), (935, 800),
            (931, 820), (925, 840), (914, 860), (898, 880), (870, 902),
            (845, 945), (820, 990), (790, 1032), (760, 1062)
        ]
    },
    # 1b. Trecho Sul do Anel Viário (avenida larga / pista dupla do Campus Central)
    {
        "name": "anel_central_boulevard",
        "width_m": 17.5,
        "is_main_route": True,
        "force_under_trees": True,
        "tight_trace": True,
        "waypoints": [
            (760, 1062), (720, 1085), (680, 1096), (640, 1100), (600, 1095),
            (565, 1084), (525, 1063), (485, 1041), (445, 1017), (405, 993),
            (370, 970), (350, 956), (328, 944), (300, 932)
        ]
    },
    # 2. Alça Oeste Externa do Viaduto / Portaria
    {
        "name": "alca_oeste_viaduto",
        "width_m": 8.5,
        "is_main_route": False,
        "force_under_trees": False,
        "waypoints": [
            (248, 1115), (268, 1040), (282, 1005), (300, 966),
            (318, 932), (342, 898), (374, 868), (400, 844)
        ]
    },
    # 3. Alças do Trevo / Viaduto (Leste e Curva Sul)
    {
        "name": "alcas_trevo_viaduto",
        "width_m": 7.5,
        "is_main_route": True,
        "force_under_trees": False,
        "tight_trace": True,
        "waypoints": [
            (384, 882), (376, 916), (373, 944), (384, 964), (408, 982)
        ]
    },
    {
        "name": "alca_sul_viaduto",
        "width_m": 8.0,
        "is_main_route": True,
        "force_under_trees": False,
        "tight_trace": True,
        "waypoints": [
            (328, 998), (350, 988), (374, 987), (400, 998), (432, 1012)
        ]
    },
    # 4. Via Interna das Escolas / Informática -> Rotatória -> Conector Norte
    {
        "name": "via_interna_escolas",
        "width_m": 8.0,
        "is_main_route": False,
        "force_under_trees": False,
        "tight_trace": True,
        "waypoints": [
            (308, 1052), (400, 1098), (480, 1140), (560, 1178),
            (598, 1192), (602, 1150), (596, 1094)
        ]
    },
    # 5a. Av. Bento Gonçalves - Pista Norte (alinhada pixel a pixel com bruto_2012_mascara)
    {
        "name": "av_bento_goncalves_norte",
        "width_m": 8.0,
        "is_main_route": True,
        "force_under_trees": True,
        "tight_trace": True,
        "waypoints": [
            (0, 1295), (50, 1294), (100, 1292), (160, 1286), (220, 1275),
            (265, 1270), (320, 1271), (360, 1275), (400, 1279), (440, 1286),
            (480, 1294), (520, 1304), (560, 1313), (600, 1324), (640, 1336),
            (680, 1350), (720, 1366), (760, 1388), (800, 1414), (840, 1446),
            (880, 1478), (905, 1491), (940, 1500), (980, 1506), (1020, 1510),
            (1060, 1514), (1100, 1518), (1140, 1522), (1180, 1532), (1220, 1549),
            (1260, 1567), (1295, 1582)
        ]
    },
    # 5b. Av. Bento Gonçalves - Pista Sul
    {
        "name": "av_bento_goncalves_sul",
        "width_m": 8.0,
        "is_main_route": True,
        "force_under_trees": True,
        "tight_trace": True,
        "waypoints": [
            (0, 1314), (50, 1313), (100, 1312), (160, 1307), (220, 1304),
            (265, 1303), (320, 1302), (360, 1301), (400, 1305), (440, 1312),
            (480, 1319), (520, 1328), (560, 1337), (600, 1348), (640, 1360),
            (680, 1374), (720, 1392), (760, 1414), (800, 1444), (840, 1478),
            (880, 1502), (905, 1509), (940, 1516), (980, 1521), (1020, 1524),
            (1060, 1527), (1100, 1530), (1140, 1534), (1180, 1545), (1220, 1562),
            (1260, 1579), (1295, 1594)
        ]
    },
    # 5c. Retornos da Rotatória Oeste e Leste na Av. Bento Gonçalves
    {
        "name": "rotatoria_bento_oeste",
        "width_m": 7.5,
        "is_main_route": True,
        "force_under_trees": True,
        "tight_trace": True,
        "waypoints": [
            (250, 1272), (236, 1279), (234, 1290), (242, 1302)
        ]
    },
    {
        "name": "rotatoria_bento_leste",
        "width_m": 7.5,
        "is_main_route": True,
        "force_under_trees": True,
        "tight_trace": True,
        "waypoints": [
            (278, 1270), (287, 1280), (286, 1293), (276, 1302)
        ]
    },
    # 6. Estrada da Lagoa / Química (Leste - traçado exato abaixo da lagoa até os prédios)
    {
        "name": "estrada_lagoa_leste",
        "width_m": 7.5,
        "is_main_route": False,
        "force_under_trees": True,
        "tight_trace": True,
        "waypoints": [
            (822, 1038), (842, 1058), (860, 1072), (880, 1084), (900, 1099),
            (920, 1114), (940, 1129), (960, 1140), (980, 1143), (1000, 1140),
            (1020, 1135), (1040, 1129), (1060, 1131), (1080, 1138), (1100, 1142),
            (1120, 1146), (1135, 1142), (1158, 1138)
        ]
    },
    # 6b. Pista Leste do Prédio Branco (Leste da Lagoa)
    {
        "name": "acesso_predio_leste",
        "width_m": 7.0,
        "is_main_route": False,
        "force_under_trees": True,
        "tight_trace": True,
        "waypoints": [
            (1134, 1142), (1138, 1100), (1145, 1054)
        ]
    },
    # 7. Pequena estrada no canto inferior direito bem coberta por árvores de ambos os lados
    {
        "name": "estrada_florestal_sul",
        "width_m": 6.5,
        "is_main_route": False,
        "force_under_trees": True,
        "waypoints": [
            (1118, 1145), (1104, 1250), (1110, 1344), (1132, 1425),
            (1140, 1475), (1148, 1525)
        ]
    }
]


def load_and_warp_satellite(tif_path: str, out_w: int, out_h: int) -> np.ndarray:
    """
    Lê a imagem de satélite mascarada via rasterio com decimação proporcional e aplica
    a transformação afim exata para o espaço métrico de `vale.png` (out_w x out_h).
    """
    with rasterio.open(tif_path) as ds:
        scale_div = max(1, min(ds.width // out_w, ds.height // out_h))
        read_w = max(out_w, ds.width // scale_div)
        read_h = max(out_h, ds.height // scale_div)
        print(f"[1/6] Lendo '{tif_path}' ({ds.width}x{ds.height}) em {read_w}x{read_h} via rasterio...")
        raw_rgb = ds.read(
            indexes=[1, 2, 3],
            out_shape=(3, read_h, read_w),
            resampling=Resampling.average
        ).transpose(1, 2, 0)

    T = AFFINE_TERRAIN_TO_TIF
    aff_coeffs = (
        T[0, 0] * (read_w / out_w), T[1, 0] * (read_w / out_h), T[2, 0] * read_w,
        T[0, 1] * (read_h / out_w), T[1, 1] * (read_h / out_h), T[2, 1] * read_h
    )

    pil_raw = Image.fromarray(raw_rgb, mode="RGB")
    warped = pil_raw.transform(
        (out_w, out_h),
        Image.Transform.AFFINE,
        aff_coeffs,
        resample=Image.Resampling.BICUBIC
    )
    return np.array(warped, dtype=np.uint8)


def extract_clean_valid_mask(sat_grid: np.ndarray) -> np.ndarray:
    """
    Extrai a máscara binária da região ativa (não-preta) de `bruto_2012_mascara.tiff`.
    Fecha apenas pequenos buracos (< 9m) causados por sombras pretas dentro da mata
    preservando integralmente os grandes recortes pretos externos e internos da rota.
    """
    rgb_sum = sat_grid.astype(np.int32).sum(axis=-1)
    raw_valid = (rgb_sum > 28).astype(np.uint8) * 255
    pil_mask = Image.fromarray(raw_valid, mode="L")
    # Fechamento morfológico leve para tapar sombras muito escuras dentro da floresta sudeste
    pil_mask = pil_mask.filter(ImageFilter.MaxFilter(size=9))
    pil_mask = pil_mask.filter(ImageFilter.MinFilter(size=9))
    # Remove ruídos isolados externos de 1-2px e apara 1px da fronteira preta
    pil_mask = pil_mask.filter(ImageFilter.MedianFilter(size=5))
    pil_mask = pil_mask.filter(ImageFilter.MinFilter(size=3))
    return np.array(pil_mask, dtype=np.uint8) > 127


def _filter_connected_components(mask: np.ndarray, min_pixels: int) -> np.ndarray:
    """Mantém apenas componentes conexos (8-vizinhança) com pelo menos `min_pixels` pixels."""
    H, W = mask.shape
    labels = np.zeros((H, W), dtype=np.int32)
    ys, xs = np.nonzero(mask)
    comp_sizes = [0]
    cid = 0
    for idx in range(len(ys)):
        sy, sx = int(ys[idx]), int(xs[idx])
        if labels[sy, sx] != 0:
            continue
        cid += 1
        q = deque([(sy, sx)])
        labels[sy, sx] = cid
        cnt = 0
        while q:
            cy, cx = q.popleft()
            cnt += 1
            for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
                ny, nx = cy + dy, cx + dx
                if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and labels[ny, nx] == 0:
                    labels[ny, nx] = cid
                    q.append((ny, nx))
        comp_sizes.append(cnt)

    comp_sizes_arr = np.array(comp_sizes, dtype=np.int32)
    return (labels > 0) & (comp_sizes_arr[labels] >= min_pixels)


def _boxblur_float(arr: np.ndarray, radius: int = 5) -> np.ndarray:
    """Box blur em imagem escalar float32 via Pillow mantendo faixa dinâmica."""
    lo, hi = float(arr.min()), float(arr.max())
    if hi - lo < 1e-6:
        return arr
    u8 = np.clip((arr - lo) / (hi - lo) * 255.0, 0.0, 255.0).astype(np.uint8)
    blurred = np.array(Image.fromarray(u8, mode="L").filter(ImageFilter.BoxBlur(radius=radius)), dtype=np.float32) / 255.0
    return blurred * (hi - lo) + lo


def extract_satellite_road_masks(sat_grid: np.ndarray, valid_mask: np.ndarray):
    """
    Extrai pistas asfaltadas diretamente dos pixels de `bruto_2012_mascara.tiff` (grid 1930x1703),
    sem usar OpenStreetMap. Envelopes largos garantem isolamento contra telhados/casas externas,
    enquanto o centro exato, largura e canteiros vêm exclusivamente da radiometria do satélite.
    Copas de árvores sobre trechos de pista são fechadas via pontes direcionais ao longo da
    tangente da pista e caminho contínuo na estrada florestal SE.
    """
    H, W, _ = sat_grid.shape
    sat_h = harmonize_southeast_seam(sat_grid, valid_mask, gains_rgb=(1.22, 1.19, 1.16)).astype(np.float32)

    R, G, B = sat_h[:, :, 0], sat_h[:, :, 1], sat_h[:, :, 2]
    lum = 0.299 * R + 0.587 * G + 0.114 * B
    chroma = np.maximum(np.maximum(R, G), B) - np.minimum(np.minimum(R, G), B)
    exg = (2.0 * G - R - B) / 255.0
    redness = (R - G) / 255.0
    warmth = (R - B) / 255.0
    blueness = (B - R) / 255.0

    pil_lum = Image.fromarray(np.clip(lum, 0.0, 255.0).astype(np.uint8), mode="L")
    lum_smooth8 = np.array(pil_lum.filter(ImageFilter.BoxBlur(radius=8)), dtype=np.float32)
    lum_smooth4 = np.array(pil_lum.filter(ImageFilter.BoxBlur(radius=4)), dtype=np.float32)
    local_var = np.abs(lum - lum_smooth4)

    yy, xx = np.mgrid[0:H, 0:W]
    ux, uy = xx.astype(np.float32) / float(W), yy.astype(np.float32) / float(H)
    in_lake = (ux > 0.455) & (ux < 0.560) & (uy > 0.550) & (uy < 0.650)
    water_raw = (
        in_lake & valid_mask & (R < 90.0) & (G < 112.0) & (lum < 105.0) &
        ((B - R) > 6.0) & ((B - G) > -6.0) & (local_var < 9.0)
    ).astype(np.uint8) * 255
    water_bin = np.array(
        Image.fromarray(water_raw, mode="L").filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MedianFilter(5))
    ) > 127

    # Telhados amplos (BoxBlur 8 separa galpões de 20x40m de pistas estreitas de 8-12px)
    roof_white = np.clip((lum - 190.0) / 22.0, 0.0, 1.0) * np.clip(1.0 - exg * 4.0, 0.0, 1.0)
    roof_smooth_grey = (
        np.clip((lum_smooth8 - 160.0) / 18.0, 0.0, 1.0) *
        np.clip(1.0 - chroma / 20.0, 0.0, 1.0) *
        np.clip((12.0 - local_var) / 6.0, 0.0, 1.0)
    )
    roof_blue_metal = np.clip((blueness - 0.075) * 8.0, 0.0, 1.0) * np.clip((lum - 102.0) / 30.0, 0.0, 1.0) * np.clip((B - G + 10.0) / 20.0, 0.0, 1.0)
    roof_red_tile = np.clip((redness - 0.075) * 9.0, 0.0, 1.0) * np.clip((warmth - 0.08) * 7.0, 0.0, 1.0) * np.clip((lum - 105.0) / 35.0, 0.0, 1.0) * np.clip((11.0 - local_var) / 6.0, 0.0, 1.0)
    bldg_raw = ((roof_white + roof_smooth_grey + roof_blue_metal + roof_red_tile) > 0.45).astype(np.uint8) * 255
    pil_bldg = Image.fromarray(bldg_raw, mode="L").filter(ImageFilter.MedianFilter(7)).filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(3))
    bldg_bin = (np.array(pil_bldg) > 127) & valid_mask & (~water_bin)

    # Envelopes amplos apenas para delimitar a área de busca do campus (jamais impõem centro/largura)
    img_roi_main = Image.new("L", (W, H), 0)
    img_roi_sec = Image.new("L", (W, H), 0)
    img_roi_bento = Image.new("L", (W, H), 0)
    d_rm = ImageDraw.Draw(img_roi_main)
    d_rs = ImageDraw.Draw(img_roi_sec)
    d_rb = ImageDraw.Draw(img_roi_bento)

    main_rois = [
        [(278, 1280), (290, 1140), (298, 1060), (345, 960), (435, 845), (505, 745), (588, 615),
         (608, 560), (596, 512), (552, 478), (516, 444), (492, 398), (488, 342), (508, 290),
         (555, 200), (595, 175), (680, 195), (765, 238), (842, 300), (886, 420), (906, 560),
         (918, 700), (926, 795), (898, 895), (815, 1020), (748, 1080), (655, 1102), (595, 1094),
         (480, 1030), (360, 965), (300, 932)],
        [(385, 880), (370, 945), (410, 990)],
        [(315, 1015), (360, 988), (430, 1010)],
    ]
    bento_roi = [
        (0, 1296), (160, 1290), (260, 1286), (380, 1288), (520, 1316), (680, 1362),
        (800, 1425), (860, 1472), (910, 1496), (980, 1510), (1050, 1518), (1180, 1538), (1300, 1588)
    ]
    sec_rois = [
        [(250, 1115), (288, 995), (344, 898), (398, 848)],
        [(308, 1052), (400, 1098), (480, 1140), (560, 1178), (598, 1192), (602, 1150), (596, 1094)],
        [(822, 1038), (940, 1125), (1040, 1134), (1135, 1140), (1152, 1138)],
        [(1136, 1140), (1142, 1085)],
    ]
    se_forest_poly = [(1118, 1145), (1104, 1250), (1110, 1344), (1132, 1425), (1140, 1475), (1148, 1525)]

    for poly in main_rois:
        d_rm.line(poly, fill=255, width=38, joint="curve")
    d_rm.line(bento_roi, fill=255, width=58, joint="curve")
    d_rb.line(bento_roi, fill=255, width=60, joint="curve")
    for poly in sec_rois:
        d_rs.line(poly, fill=255, width=32, joint="curve")
    d_rs.line(se_forest_poly, fill=255, width=24, joint="curve")

    roi_main = np.array(img_roi_main) > 127
    roi_bento = np.array(img_roi_bento) > 127
    roi_sec = (np.array(img_roi_sec) > 127) & (~roi_main)
    roi_any = roi_main | roi_sec

    # 1. Asfalto visível direto do satélite (cinza neutro a concreto de viaduto/ponte)
    asph_base = (chroma < 26.0) & (exg < 0.025) & (warmth < 0.075) & (lum >= 62.0) & (lum <= 186.0)
    asph_bridge_conc = (chroma < 20.0) & (exg < 0.015) & (warmth < 0.050) & (lum > 186.0) & (lum <= 204.0)
    asph_strict = roi_any & valid_mask & (~water_bin) & (~bldg_bin) & (asph_base | asph_bridge_conc)

    near_strict = np.array(Image.fromarray(asph_strict.astype(np.uint8) * 255, mode="L").filter(ImageFilter.MaxFilter(5))) > 127
    asph_shadow = (
        near_strict & roi_any & valid_mask & (~water_bin) & (~bldg_bin) &
        (chroma < 20.0) & (exg < 0.015) & (warmth < 0.055) & (lum >= 38.0) & (lum < 62.0)
    )
    pil_a = Image.fromarray((asph_strict | asph_shadow).astype(np.uint8) * 255, mode="L")
    pil_a = pil_a.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3))
    pil_a = pil_a.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(3))
    cand_clean = (np.array(pil_a) > 127) & valid_mask & (~water_bin) & (~bldg_bin)
    road_vis = _filter_connected_components(cand_clean, min_pixels=65)

    # 2. Tensor de estrutura para estimar a tangente local da pista e fechar trechos sob árvores
    rv_blur = np.array(Image.fromarray(road_vis.astype(np.uint8) * 255, mode="L").filter(ImageFilter.GaussianBlur(radius=3.0)), dtype=np.float32) / 255.0
    gx = np.zeros_like(rv_blur)
    gy = np.zeros_like(rv_blur)
    gx[:, 1:-1] = (rv_blur[:, 2:] - rv_blur[:, :-2]) * 0.5
    gy[1:-1, :] = (rv_blur[2:, :] - rv_blur[:-2, :]) * 0.5

    Jxx = _boxblur_float(gx * gx, 5)
    Jxy = _boxblur_float(gx * gy, 5)
    Jyy = _boxblur_float(gy * gy, 5)
    tang_ang = 0.5 * np.arctan2(2.0 * Jxy, Jxx - Jyy) + 0.5 * math.pi
    tx = np.cos(tang_ang)
    ty = np.sin(tang_ang)

    # Protege gramado ensolarado e especialmente o canteiro central da Av. Bento Gonçalves
    sunlit_grass = (exg > 0.145) & (lum > 118.0)
    bento_median_grass = roi_bento & (exg > 0.060) & (lum > 72.0)
    near_rv = np.array(Image.fromarray(road_vis.astype(np.uint8) * 255, mode="L").filter(ImageFilter.MaxFilter(45))) > 127
    cand_bridge = roi_any & valid_mask & (~water_bin) & (~bldg_bin) & (~road_vis) & (~sunlit_grass) & (~bento_median_grass) & near_rv

    cy_idx, cx_idx = np.nonzero(cand_bridge)
    bridged = np.zeros((H, W), dtype=bool)
    angles = np.linspace(0.0, math.pi, 18, endpoint=False)
    max_half = 24

    for ang in angles:
        ux_a, uy_a = math.cos(ang), math.sin(ang)
        n_cand = len(cy_idx)
        has_fwd = np.zeros(n_cand, dtype=bool)
        has_bwd = np.zeros(n_cand, dtype=bool)
        df = np.full(n_cand, 999, dtype=np.int16)
        db = np.full(n_cand, 999, dtype=np.int16)
        tf_ok = np.zeros(n_cand, dtype=bool)
        tb_ok = np.zeros(n_cand, dtype=bool)
        tfx = np.zeros(n_cand, dtype=np.float32)
        tfy = np.zeros(n_cand, dtype=np.float32)
        tbx = np.zeros(n_cand, dtype=np.float32)
        tby = np.zeros(n_cand, dtype=np.float32)

        for d in range(2, max_half + 1):
            nx = np.clip(np.round(cx_idx + d * ux_a).astype(np.int32), 0, W - 1)
            ny = np.clip(np.round(cy_idx + d * uy_a).astype(np.int32), 0, H - 1)
            hit = road_vis[ny, nx] & (~has_fwd)
            if np.any(hit):
                hx, hy = tx[ny[hit], nx[hit]], ty[ny[hit], nx[hit]]
                has_fwd[hit] = True
                df[hit] = d
                tf_ok[hit] = np.abs(hx * ux_a + hy * uy_a) >= 0.905
                tfx[hit], tfy[hit] = hx, hy

            nx2 = np.clip(np.round(cx_idx - d * ux_a).astype(np.int32), 0, W - 1)
            ny2 = np.clip(np.round(cy_idx - d * uy_a).astype(np.int32), 0, H - 1)
            hit2 = road_vis[ny2, nx2] & (~has_bwd)
            if np.any(hit2):
                hx2, hy2 = tx[ny2[hit2], nx2[hit2]], ty[ny2[hit2], nx2[hit2]]
                has_bwd[hit2] = True
                db[hit2] = d
                tb_ok[hit2] = np.abs(hx2 * ux_a + hy2 * uy_a) >= 0.905
                tbx[hit2], tby[hit2] = hx2, hy2

        pair_dot = np.abs(tfx * tbx + tfy * tby)
        valid_hit = tf_ok & tb_ok & (pair_dot >= 0.84) & ((df + db) <= 44)
        if np.any(valid_hit):
            bridged[cy_idx[valid_hit], cx_idx[valid_hit]] = True

    # 3. Estrada florestal SE densamente arborizada (Dijkstra rápido passando pelas brechas visíveis)
    img_se_cor = Image.new("L", (W, H), 0)
    ImageDraw.Draw(img_se_cor).line(se_forest_poly, fill=255, width=20, joint="curve")
    se_cor = np.array(img_se_cor) > 127
    se_cost = np.full((H, W), 100.0, dtype=np.float32)
    se_cost[se_cor] = 1.0 - 0.70 * road_vis[se_cor].astype(np.float32) + np.clip(exg[se_cor] * 1.5, 0.0, 0.6)
    se_cost[bldg_bin] = 50.0

    se_path = []
    for (sx, sy), (ex, ey) in zip(se_forest_poly[:-1], se_forest_poly[1:]):
        y_min = max(0, min(sy, ey) - 14)
        y_max = min(H - 1, max(sy, ey) + 14)
        x_min = max(0, min(sx, ex) - 14)
        x_max = min(W - 1, max(sx, ex) + 14)
        pq = [(0.0, sy, sx)]
        dist = {(sy, sx): 0.0}
        parent = {}
        while pq:
            d, y, x = heapq.heappop(pq)
            if x == ex and y == ey:
                break
            if d > dist.get((y, x), 1e9):
                continue
            for dy, dx, step in ((-1, 0, 1.0), (1, 0, 1.0), (0, -1, 1.0), (0, 1, 1.0), (-1, -1, 1.4142), (-1, 1, 1.4142), (1, -1, 1.4142), (1, 1, 1.4142)):
                ny, nx = y + dy, x + dx
                if y_min <= ny <= y_max and x_min <= nx <= x_max and se_cost[ny, nx] < 40.0:
                    nd = d + step * float(se_cost[ny, nx])
                    if nd < dist.get((ny, nx), 1e9):
                        dist[(ny, nx)] = nd
                        parent[(ny, nx)] = (y, x)
                        heapq.heappush(pq, (nd, ny, nx))
        seg = []
        cur = (ey, ex)
        while cur in parent:
            seg.append((cur[1], cur[0]))
            cur = parent[cur]
        seg.append((sx, sy))
        seg.reverse()
        se_path.extend(seg[:-1])
    se_path.append(se_forest_poly[-1])

    img_se_ribbon = Image.new("L", (W, H), 0)
    ImageDraw.Draw(img_se_ribbon).line(se_path, fill=255, width=9, joint="curve")
    se_ribbon = (np.array(img_se_ribbon) > 127) & valid_mask & (~bldg_bin) & (~water_bin)

    img_se_band = Image.new("L", (W, H), 0)
    ImageDraw.Draw(img_se_band).line(se_path, fill=255, width=15, joint="curve")
    img_se_wide = Image.new("L", (W, H), 0)
    ImageDraw.Draw(img_se_wide).line(se_forest_poly, fill=255, width=24, joint="curve")
    img_lagoa = Image.new("L", (W, H), 0)
    ImageDraw.Draw(img_lagoa).line(sec_rois[2], fill=255, width=32, joint="curve")
    se_noise_kill = (np.array(img_se_wide) > 127) & (~(np.array(img_se_band) > 127)) & (~roi_main) & (~(np.array(img_lagoa) > 127))

    pil_br = Image.fromarray(bridged.astype(np.uint8) * 255, mode="L").filter(ImageFilter.MedianFilter(3))
    bridged_clean = ((np.array(pil_br) > 127) | se_ribbon) & (~road_vis) & valid_mask & (~water_bin) & (~bldg_bin)
    road_raw = (road_vis | bridged_clean) & (~se_noise_kill) | se_ribbon
    pil_rf = Image.fromarray(road_raw.astype(np.uint8) * 255, mode="L").filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3))
    road_clean = (np.array(pil_rf) > 127) & valid_mask & (~water_bin) & (~bldg_bin)

    # Remove pequenas manchas isoladas (pedras/telhados perdidos < 220 px), mantendo toda a rede viária
    road_final = _filter_connected_components(road_clean, min_pixels=220)
    cv_main_grid = road_final & roi_main
    cv_sec_grid = road_final & (~roi_main)
    cv_bridge_grid = ((road_final & (~road_vis)) | se_ribbon) & road_final
    return cv_main_grid, cv_sec_grid, cv_bridge_grid, road_final, se_path


def snap_corridors_dijkstra(
    sat_grid: np.ndarray,
    valid_mask: np.ndarray,
    road_cv_grid: np.ndarray = None,
    se_path_override=None
):
    """
    Ajusta os corredores usados no nivelamento altimétrico (`vale_processed.png`) aos
    pixels exatos de pista extraídos do satélite (`road_cv_grid`).
    """
    H, W, _ = sat_grid.shape
    if road_cv_grid is not None:
        asph_raw = road_cv_grid.astype(np.float32)
    else:
        sat_f = sat_grid.astype(np.float32)
        R, G, B = sat_f[:, :, 0], sat_f[:, :, 1], sat_f[:, :, 2]
        lum = 0.299 * R + 0.587 * G + 0.114 * B
        chroma = np.maximum(np.maximum(R, G), B) - np.minimum(np.minimum(R, G), B)
        exg = (2.0 * G - R - B) / 255.0
        warmth = (R - B) / 255.0
        asph_raw = (
            valid_mask.astype(np.float32) *
            np.clip(1.0 - chroma / 22.0, 0.0, 1.0) *
            np.clip((lum - 74.0) / 20.0, 0.0, 1.0) *
            np.clip((188.0 - lum) / 14.0, 0.0, 1.0) *
            np.clip((0.032 - exg) / 0.045, 0.0, 1.0) *
            np.clip((0.08 - np.abs(warmth)) / 0.06, 0.0, 1.0)
        )
    road_ridge = np.array(
        Image.fromarray(np.clip(asph_raw * 255.0, 0, 255).astype(np.uint8), mode="L").filter(ImageFilter.GaussianBlur(radius=2.0)),
        dtype=np.float32
    ) / 255.0

    def snap_pt(px: int, py: int, rad: int = 12):
        x0, x1 = max(0, px - rad), min(W - 1, px + rad)
        y0, y1 = max(0, py - rad), min(H - 1, py + rad)
        sub = road_ridge[y0:y1 + 1, x0:x1 + 1]
        if float(np.max(sub)) <= 0.42:
            return px, py
        yy, xx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
        dist_p = np.hypot(xx - px, yy - py)
        score = sub - 0.012 * dist_p
        iy, ix = np.unravel_index(np.argmax(score), score.shape)
        return int(x0 + ix), int(y0 + iy)

    cost_map = np.where(~valid_mask, 40.0, 1.05 - 0.98 * np.clip(road_ridge * 1.4, 0.0, 1.0)).astype(np.float32)

    neighbors = [
        (-1, 0, 1.0), (1, 0, 1.0), (0, -1, 1.0), (0, 1, 1.0),
        (-1, -1, 1.4142), (1, -1, 1.4142), (-1, 1, 1.4142), (1, 1, 1.4142)
    ]

    snapped_corridors = []
    for corr in CAMPUS_ROAD_CORRIDORS:
        if corr["name"] == "estrada_florestal_sul" and se_path_override:
            snapped_corridors.append({
                "name": corr["name"],
                "width_m": corr["width_m"],
                "is_main_route": corr["is_main_route"],
                "force_under_trees": True,
                "points": [(float(p[0]), float(p[1])) for p in se_path_override]
            })
            continue
        waypoints = corr["waypoints"]
        is_hidden = corr.get("force_under_trees", False)
        is_tight = corr.get("tight_trace", False)
        if is_tight:
            if not is_hidden:
                waypoints = [snap_pt(px, py, rad=2) for (px, py) in waypoints]
            margin = 5
            cross_w = 0.042
        else:
            if not is_hidden:
                waypoints = [snap_pt(px, py, rad=10) for (px, py) in waypoints]
            margin = 6 if is_hidden else 18
            cross_w = 0.040 if is_hidden else 0.014
        full_pts = []

        for i in range(len(waypoints) - 1):
            x0, y0 = waypoints[i]
            x1, y1 = waypoints[i + 1]
            xmin = max(0, min(x0, x1) - margin)
            xmax = min(W - 1, max(x0, x1) + margin)
            ymin = max(0, min(y0, y1) - margin)
            ymax = min(H - 1, max(y0, y1) + margin)
            sub_w = xmax - xmin + 1
            sub_h = ymax - ymin + 1
            sx, sy = x0 - xmin, y0 - ymin
            gx, gy = x1 - xmin, y1 - ymin

            dist = np.full((sub_h, sub_w), 1e9, dtype=np.float32)
            parent = np.full((sub_h, sub_w, 2), -1, dtype=np.int32)
            dist[sy, sx] = 0.0
            pq = [(0.0, sx, sy)]
            seg_len = max(1.0, math.hypot(x1 - x0, y1 - y0))

            while pq:
                d, cx, cy = heapq.heappop(pq)
                if cx == gx and cy == gy:
                    break
                if d > dist[cy, cx]:
                    continue
                for dx, dy, step in neighbors:
                    nx, ny = cx + dx, cy + dy
                    if 0 <= nx < sub_w and 0 <= ny < sub_h:
                        wx, wy = nx + xmin, ny + ymin
                        cross = abs((x1 - x0) * (y0 - wy) - (x0 - wx) * (y1 - y0)) / seg_len
                        c = float(cost_map[wy, wx]) + cross_w * cross
                        nd = d + step * c
                        if nd < dist[ny, nx]:
                            dist[ny, nx] = nd
                            parent[ny, nx] = [cx, cy]
                            heapq.heappush(pq, (nd, nx, ny))

            seg = []
            cx, cy = gx, gy
            while cx != -1 and cy != -1:
                seg.append((float(cx + xmin), float(cy + ymin)))
                px, py = parent[cy, cx]
                cx, cy = int(px), int(py)
            seg.reverse()
            if full_pts:
                full_pts.extend(seg[1:])
            else:
                full_pts.extend(seg)

        pts_arr = np.array(full_pts, dtype=np.float32)
        if len(pts_arr) > 9:
            kernel = np.ones(7, dtype=np.float32) / 7.0
            pts_arr[3:-3, 0] = np.convolve(pts_arr[:, 0], kernel, mode="valid")
            pts_arr[3:-3, 1] = np.convolve(pts_arr[:, 1], kernel, mode="valid")

        snapped_corridors.append({
            "name": corr["name"],
            "width_m": corr["width_m"],
            "is_main_route": corr["is_main_route"],
            "force_under_trees": corr.get("force_under_trees", False),
            "points": [(float(p[0]), float(p[1])) for p in pts_arr]
        })

    return snapped_corridors


def rasterize_road_masks(snapped_corridors, w: int, h: int, scale_x: float = 1.0, scale_y: float = 1.0):
    """
    Rasteriza máscaras para vias principais, secundárias, vias ocultas sob árvores,
    borda de calçada junto às vias e zona de exclusão de árvores ao redor das pistas.
    """
    img_main = Image.new("L", (w, h), 0)
    img_sec = Image.new("L", (w, h), 0)
    img_hidden = Image.new("L", (w, h), 0)
    img_sidewalk_edge = Image.new("L", (w, h), 0)
    img_clear = Image.new("L", (w, h), 0)

    draw_main = ImageDraw.Draw(img_main)
    draw_sec = ImageDraw.Draw(img_sec)
    draw_hidden = ImageDraw.Draw(img_hidden)
    draw_sw = ImageDraw.Draw(img_sidewalk_edge)
    draw_clear = ImageDraw.Draw(img_clear)

    avg_scale = 0.5 * (scale_x + scale_y)
    for corr in snapped_corridors:
        pts = [(p[0] * scale_x, p[1] * scale_y) for p in corr["points"]]
        road_w = max(2, int(round(corr["width_m"] * avg_scale)))
        edge_w = max(3, int(round((corr["width_m"] + 4.0) * avg_scale)))
        clear_w = max(5, int(round((corr["width_m"] + 9.0) * avg_scale)))

        draw_clear.line(pts, fill=255, width=clear_w, joint="curve")
        draw_sw.line(pts, fill=255, width=edge_w, joint="curve")
        if corr["is_main_route"]:
            draw_main.line(pts, fill=255, width=road_w, joint="curve")
        else:
            draw_sec.line(pts, fill=255, width=road_w, joint="curve")
        if corr.get("force_under_trees", False):
            draw_hidden.line(pts, fill=255, width=road_w, joint="curve")

    blur_r = max(1.0, 1.0 * avg_scale)
    main_mask = np.array(img_main.filter(ImageFilter.GaussianBlur(radius=blur_r)), dtype=np.float32) / 255.0
    sec_mask = np.array(img_sec.filter(ImageFilter.GaussianBlur(radius=blur_r)), dtype=np.float32) / 255.0
    hidden_mask = np.array(img_hidden.filter(ImageFilter.GaussianBlur(radius=blur_r)), dtype=np.float32) / 255.0
    edge_ring = np.clip(
        np.array(img_sidewalk_edge.filter(ImageFilter.GaussianBlur(radius=blur_r)), dtype=np.float32) / 255.0
        - np.maximum(main_mask, sec_mask),
        0.0,
        1.0
    )
    clear_mask = np.array(img_clear, dtype=np.uint8) > 64

    return main_mask, sec_mask, hidden_mask, edge_ring, clear_mask


def harmonize_southeast_seam(sat_rgb: np.ndarray, valid_mask: np.ndarray, gains_rgb=(1.20, 1.17, 1.15)) -> np.ndarray:
    """
    Suaviza a diferença de exposição no mosaico sudeste do satélite.
    """
    H, W, _ = sat_rgb.shape
    out = sat_rgb.astype(np.float32).copy()

    x0, x1 = int(0.548 * W), int(0.840 * W)
    y0, y1 = int(0.568 * H), int(0.950 * H)

    tile_mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(tile_mask).rectangle([x0, y0, x1, y1], fill=255)
    feather = np.array(tile_mask.filter(ImageFilter.GaussianBlur(radius=max(6, W // 160))), dtype=np.float32) / 255.0
    feather *= valid_mask.astype(np.float32)

    gains = np.array(gains_rgb, dtype=np.float32)
    for c in range(3):
        out[:, :, c] = out[:, :, c] * (1.0 + feather * (gains[c] - 1.0))

    return np.clip(out, 0.0, 255.0).astype(np.uint8)


def classify_terrain_and_build_splatmaps(
    sat_2k: np.ndarray,
    valid_2k: np.ndarray,
    main_road_2k: np.ndarray,
    sec_road_2k: np.ndarray,
    hidden_road_2k: np.ndarray,
    edge_ring_2k: np.ndarray
):
    """
    Segmenta a área útil da ortofoto nos 8 materiais para aplicar as texturas
    placeholder correspondentes em cada região:
      0: Asfalto Principal
      1: Asfalto Secundário / Estacionamentos / Concreto Urbano
      2: Calçadas / Piso Pavimentado
      3: Grama Aberta / Canteiros
      4: Solo de Mata / Serapilheira
      5: Terra Exposta / Taludes / Saibro
      6: Água (Lagoa do Vale - recebe textura placeholder de água, sem shader de ondas)
      7: Fundação / Base de Prédios
    """
    H, W, _ = sat_2k.shape
    sat_f = harmonize_southeast_seam(sat_2k, valid_2k).astype(np.float32)
    R, G, B = sat_f[:, :, 0], sat_f[:, :, 1], sat_f[:, :, 2]

    lum = 0.299 * R + 0.587 * G + 0.114 * B
    max_c = np.maximum(np.maximum(R, G), B)
    min_c = np.minimum(np.minimum(R, G), B)
    chroma = max_c - min_c
    exg = (2.0 * G - R - B) / 255.0
    redness = (R - G) / 255.0
    warmth = (R - B) / 255.0
    blueness = (B - R) / 255.0

    # Variação de textura local (telhados/água são mais lisos; copas de árvores são rugosas)
    pil_lum = Image.fromarray(np.clip(lum, 0, 255).astype(np.uint8), mode="L")
    lum_smooth = np.array(pil_lum.filter(ImageFilter.BoxBlur(radius=4)), dtype=np.float32)
    lum_smooth8 = np.array(pil_lum.filter(ImageFilter.BoxBlur(radius=8)), dtype=np.float32)
    local_var = np.abs(lum - lum_smooth)

    road_corridor = np.clip(main_road_2k + sec_road_2k, 0.0, 1.0)

    # 1. Água (Lagoa do Vale: azul-esverdeado escuro onde B > R+6 e B >= G-6, liso)
    yy, xx = np.mgrid[0:H, 0:W]
    ux = xx.astype(np.float32) / float(W)
    uy = yy.astype(np.float32) / float(H)
    in_lake_basin = (ux > 0.455) & (ux < 0.560) & (uy > 0.550) & (uy < 0.650)
    water_raw = (
        in_lake_basin &
        valid_2k &
        (R < 90.0) &
        (G < 112.0) &
        (lum < 105.0) &
        ((B - R) > 6.0) &
        ((B - G) > -6.0) &
        (local_var < 9.0) &
        (road_corridor < 0.20)
    ).astype(np.uint8) * 255
    pil_water = Image.fromarray(water_raw, mode="L")
    pil_water = pil_water.filter(ImageFilter.MaxFilter(size=5))
    pil_water = pil_water.filter(ImageFilter.MinFilter(size=5))
    pil_water = pil_water.filter(ImageFilter.MedianFilter(size=5))
    water_bin = np.array(pil_water, dtype=np.uint8) > 127
    w_water = (np.array(pil_water.filter(ImageFilter.GaussianBlur(radius=1.5)), dtype=np.float32) / 255.0) * 8.0

    # 2. Vegetação (grama aberta e copa/mata)
    veg_score = np.clip((exg - 0.032) * 8.5, 0.0, 1.0) * (1.0 - np.clip(w_water, 0.0, 1.0))

    # 3. Prédios / Telhados (brancos, zinco cinza claro amplo, zinco azulado, telha cerâmica)
    not_road_or_water = np.clip(1.0 - road_corridor * 1.6, 0.0, 1.0) * np.clip(1.0 - w_water, 0.0, 1.0)
    roof_white = np.clip((lum - 190.0) / 22.0, 0.0, 1.0) * np.clip(1.0 - exg * 4.0, 0.0, 1.0)
    roof_smooth_grey = (
        np.clip((lum_smooth8 - 144.0) / 18.0, 0.0, 1.0) *
        np.clip(1.0 - chroma / 18.0, 0.0, 1.0) *
        np.clip((12.5 - local_var) / 6.0, 0.0, 1.0)
    )
    roof_blue_metal = (
        np.clip((blueness - 0.075) * 8.0, 0.0, 1.0) *
        np.clip((lum - 102.0) / 30.0, 0.0, 1.0) *
        np.clip((B - G + 10.0) / 20.0, 0.0, 1.0)
    )
    roof_red_tile = (
        np.clip((redness - 0.075) * 9.0, 0.0, 1.0) *
        np.clip((warmth - 0.08) * 7.0, 0.0, 1.0) *
        np.clip((lum - 105.0) / 35.0, 0.0, 1.0) *
        np.clip((11.0 - local_var) / 6.0, 0.0, 1.0)
    )
    bldg_raw = ((roof_white + roof_smooth_grey + roof_blue_metal + roof_red_tile) * not_road_or_water > 0.45).astype(np.uint8) * 255
    pil_bldg = Image.fromarray(bldg_raw, mode="L")
    # Limpa ruídos e consolida os retângulos dos prédios
    pil_bldg = pil_bldg.filter(ImageFilter.MedianFilter(size=7))
    pil_bldg = pil_bldg.filter(ImageFilter.MaxFilter(size=5))
    pil_bldg = pil_bldg.filter(ImageFilter.MinFilter(size=3))
    bldg_mask_bin = (np.array(pil_bldg, dtype=np.uint8) > 127) & valid_2k & (road_corridor < 0.25) & (~water_bin)
    w_building = (np.array(pil_bldg.filter(ImageFilter.GaussianBlur(radius=1.5)), dtype=np.float32) / 255.0) * 5.0
    w_building *= not_road_or_water

    # 4. Terra Exposta / Barro Vermelho / Taludes / Saibro
    w_dirt = (
        np.clip((warmth - 0.032) * 11.0, 0.0, 1.0) *
        np.clip((redness - 0.015) * 14.0, 0.0, 1.0) *
        np.clip(1.0 - veg_score * 1.6, 0.0, 1.0) *
        (1.0 - np.clip(w_building, 0.0, 1.0)) *
        np.clip(1.0 - road_corridor * 1.4, 0.0, 1.0)
    ) * 3.8

    # 5. Estacionamentos escuros visíveis fora de prédios e das vias principais
    vis_asphalt_raw = (
        valid_2k &
        (~water_bin) &
        (~bldg_mask_bin) &
        (chroma < 18.0) &
        (lum >= 80.0) & (lum <= 136.0) &
        (exg < 0.015) &
        (warmth < 0.060)
    )
    pil_va_open = Image.fromarray(vis_asphalt_raw.astype(np.uint8) * 255, mode="L")
    pil_va_open = pil_va_open.filter(ImageFilter.MedianFilter(size=5)).filter(ImageFilter.MinFilter(size=5)).filter(ImageFilter.MaxFilter(size=5))
    vis_asphalt_lots = (np.array(pil_va_open.filter(ImageFilter.GaussianBlur(radius=1.0)), dtype=np.float32) / 255.0)
    vis_asphalt_lots *= (1.0 - np.clip(w_building * 0.8, 0.0, 1.0)) * (1.0 - np.clip(w_water, 0.0, 1.0)) * (1.0 - np.clip(road_corridor * 1.4, 0.0, 1.0))

    # Pistas principais e secundárias extraídas pixel a pixel do satélite (+ pontes sob árvores)
    w_asphalt_main = main_road_2k * 4.6
    w_asphalt_sec = np.maximum(
        sec_road_2k * 4.2,
        vis_asphalt_lots * (1.0 - np.clip(main_road_2k * 1.5, 0.0, 1.0)) * 2.4
    )

    # 6. Calçadas / Piso Pavimentado (pátios claros + borda estreita junto às vias extraídas)
    vis_sidewalk = (
        np.clip(1.0 - chroma / 26.0, 0.0, 1.0) *
        np.clip((lum - 142.0) / 24.0, 0.0, 1.0) *
        np.clip((198.0 - lum) / 22.0, 0.0, 1.0) *
        np.clip(1.0 - veg_score * 1.5, 0.0, 1.0) *
        np.clip(1.0 - w_dirt * 0.6, 0.0, 1.0) *
        np.clip(1.0 - w_building * 0.5, 0.0, 1.0) *
        np.clip(1.0 - road_corridor * 1.5, 0.0, 1.0)
    )
    edge_sidewalk = edge_ring_2k * np.clip(1.0 - veg_score * 0.85, 0.20, 1.0) * (1.0 - np.clip(w_building * 0.6, 0.0, 1.0))
    w_sidewalk = np.maximum(vis_sidewalk * 2.4, edge_sidewalk * 1.7)

    # 7. Grama Aberta vs Solo de Mata (Serapilheira)
    unocc_total = np.clip(hidden_road_2k, 0.0, 1.0)
    canopy_suppression = np.clip(1.0 - road_corridor * 1.6, 0.0, 1.0)
    w_grass = (
        veg_score *
        np.clip((lum - 90.0) / 38.0, 0.12, 1.0) *
        np.clip(1.0 - w_building * 0.6, 0.0, 1.0) *
        canopy_suppression * 2.8
    )
    w_forest = (
        np.maximum(
            veg_score * np.clip((114.0 - lum) / 36.0, 0.0, 1.0),
            np.clip((88.0 - lum) / 35.0, 0.0, 1.0) * (1.0 - np.clip(w_water, 0.0, 1.0))
        ) * canopy_suppression * np.clip(1.0 - w_building * 0.6, 0.0, 1.0) * 2.5
    )

    # Fallback mínimo de grama caso algum pixel válido tenha pesos zerados
    w_grass = np.maximum(w_grass, 0.04)

    weights = np.stack([
        w_asphalt_main,  # Canal 0: splatmap_0.r -> mat_0_asphalt_main
        w_asphalt_sec,   # Canal 1: splatmap_0.g -> mat_1_asphalt_sec
        w_sidewalk,      # Canal 2: splatmap_0.b -> mat_2_sidewalk
        w_grass,         # Canal 3: splatmap_0.a -> mat_3_grass
        w_forest,        # Canal 4: splatmap_1.r -> mat_4_forest
        w_dirt,          # Canal 5: splatmap_1.g -> mat_5_dirt
        w_water,         # Canal 6: splatmap_1.b -> mat_6_water
        w_building       # Canal 7: splatmap_1.a -> mat_7_building
    ], axis=-1)

    # Suavização leve para transições naturais entre materiais
    for c in range(8):
        ch_u8 = np.clip(weights[:, :, c] / max(1.0, float(weights[:, :, c].max())) * 255.0, 0, 255).astype(np.uint8)
        # Normaliza pelo fator usado acima antes de borramento suave
        pass

    # Borramento uniforme preservando a magnitude relativa entre canais
    for c in range(8):
        vmax = float(np.max(weights[:, :, c]))
        if vmax < 1e-5:
            continue
        scaled = np.clip(weights[:, :, c] / vmax * 255.0, 0, 255).astype(np.uint8)
        blurred = np.array(Image.fromarray(scaled, mode="L").filter(ImageFilter.GaussianBlur(radius=1.2)), dtype=np.float32) / 255.0
        weights[:, :, c] = blurred * vmax

    total = np.maximum(np.sum(weights, axis=-1, keepdims=True), 1e-5)
    weights = weights / total
    weights[~valid_2k] = 0.0

    splat0 = np.clip(np.round(weights[:, :, 0:4] * 255.0), 0, 255).astype(np.uint8)
    splat1 = np.clip(np.round(weights[:, :, 4:8] * 255.0), 0, 255).astype(np.uint8)

    # Máscara de copa de árvore para amostragem de árvores 3D (fora das vias, prédios e água)
    canopy_mask_2k = (
        valid_2k &
        (~water_bin) &
        (~bldg_mask_bin) &
        (road_corridor < 0.08) &
        (((exg > 0.055) & (lum < 124.0)) | ((lum < 85.0) & (exg > 0.015)))
    )

    return splat0, splat1, unocc_total, water_bin, bldg_mask_bin, canopy_mask_2k


def build_macro_albedo_4k(
    sat_4k: np.ndarray,
    valid_2k: np.ndarray,
    unocc_2k: np.ndarray,
    bldg_2k: np.ndarray
) -> np.ndarray:
    """
    Constrói `macro_albedo.png` (4096 x 4096 RGB) como modulador cromático limpo:
      - Harmoniza a emenda do quadrante sudeste;
      - Suaviza áreas de prédios e trechos de rua ocultos para não vazar cor de
        telhado ou copa escura sobre as texturas placeholder;
      - Mantém preto puro (0, 0, 0) fora da máscara válida.
    """
    H4, W4, _ = sat_4k.shape
    valid_4k = np.array(
        Image.fromarray(valid_2k.astype(np.uint8) * 255, mode="L").resize((W4, H4), resample=Image.Resampling.BILINEAR),
        dtype=np.uint8
    ) > 127

    harmonized = harmonize_southeast_seam(sat_4k, valid_4k).astype(np.float32)

    # Suaviza levemente em baixa frequência para remover carros estacionados e manter só variações regionais de cor
    smooth_bg = np.array(
        Image.fromarray(harmonized.astype(np.uint8), mode="RGB").filter(ImageFilter.GaussianBlur(radius=6.0)),
        dtype=np.float32
    )
    # Despintura de vias ocultas
    unocc_4k = np.array(
        Image.fromarray(np.clip(unocc_2k * 255.0, 0, 255).astype(np.uint8), mode="L").resize((W4, H4), resample=Image.Resampling.BILINEAR),
        dtype=np.float32
    ) / 255.0
    bldg_4k = np.array(
        Image.fromarray(bldg_2k.astype(np.uint8) * 255, mode="L").resize((W4, H4), resample=Image.Resampling.BILINEAR),
        dtype=np.float32
    ) / 255.0

    macro = harmonized * 0.45 + smooth_bg * 0.55
    neutral_pad = np.array([145.0, 145.0, 145.0], dtype=np.float32)
    for c in range(3):
        macro[:, :, c] = macro[:, :, c] * (1.0 - unocc_4k * 0.65) + 128.0 * (unocc_4k * 0.65)
        macro[:, :, c] = macro[:, :, c] * (1.0 - bldg_4k * 0.85) + neutral_pad[c] * (bldg_4k * 0.85)

    macro[~valid_4k] = 0.0
    return np.clip(np.round(macro), 0, 255).astype(np.uint8)


def process_heightmap_16bit(
    vale_png_path: str,
    snapped_corridors,
    water_bin_2k: np.ndarray,
    out_path: str
) -> np.ndarray:
    """
    Refina o heightmap 16-bit (`data/vale.png` -> `data/vale_processed.png`, 1930x1703):
      1. Aplaina a inclinação transversal (cross-slope) das vias preservando as subidas/descidas;
      2. Nivela o espelho da Lagoa do Vale.
    """
    img_16 = Image.open(vale_png_path)
    h_raw = np.array(img_16, dtype=np.uint16)
    H, W = h_raw.shape
    h_float = h_raw.astype(np.float32)

    target_sum = np.zeros((H, W), dtype=np.float32)
    weight_sum = np.zeros((H, W), dtype=np.float32)

    for corr in snapped_corridors:
        pts = np.array(corr["points"], dtype=np.float32)
        if len(pts) < 4:
            continue
        px = np.clip(np.round(pts[:, 0]).astype(np.int32), 0, W - 1)
        pz = np.clip(np.round(pts[:, 1]).astype(np.int32), 0, H - 1)
        center_h = h_float[pz, px]

        k_len = min(15, len(center_h))
        if k_len >= 5:
            kernel = np.ones(k_len, dtype=np.float32) / float(k_len)
            pad = k_len // 2
            padded = np.pad(center_h, (pad, pad), mode="edge")
            center_h = np.convolve(padded, kernel, mode="valid")[:len(center_h)]

        half_w = 0.5 * corr["width_m"]
        shoulder_w = half_w + 5.0
        r_int = int(math.ceil(shoulder_w))

        for idx in range(len(pts)):
            cx, cz = pts[idx, 0], pts[idx, 1]
            ch = center_h[idx]
            x0 = max(0, int(cx) - r_int)
            x1 = min(W - 1, int(cx) + r_int)
            z0 = max(0, int(cz) - r_int)
            z1 = min(H - 1, int(cz) + r_int)

            yy, xx = np.mgrid[z0:z1 + 1, x0:x1 + 1]
            dist = np.hypot(xx - cx, yy - cz)
            w_local = np.where(
                dist <= half_w,
                1.0,
                np.where(
                    dist < shoulder_w,
                    0.5 * (1.0 + np.cos(math.pi * (dist - half_w) / (shoulder_w - half_w))),
                    0.0
                )
            ).astype(np.float32)

            target_sum[z0:z1 + 1, x0:x1 + 1] += ch * w_local
            weight_sum[z0:z1 + 1, x0:x1 + 1] += w_local

    has_road = weight_sum > 1e-4
    road_target = np.where(has_road, target_sum / np.maximum(weight_sum, 1e-4), h_float)
    blend_alpha = np.clip(weight_sum, 0.0, 1.0) * 0.85
    h_leveled = h_float * (1.0 - blend_alpha) + road_target * blend_alpha

    # Nivelamento plano da bacia da Lagoa do Vale
    water_grid = np.array(
        Image.fromarray(water_bin_2k.astype(np.uint8) * 255, mode="L").resize((W, H), resample=Image.Resampling.BILINEAR),
        dtype=np.float32
    ) / 255.0
    if np.any(water_grid > 0.5):
        lake_level = float(np.percentile(h_leveled[water_grid > 0.5], 20.0)) - 100.0
        h_leveled = h_leveled * (1.0 - water_grid * 0.9) + lake_level * (water_grid * 0.9)

    h_out = np.clip(np.round(h_leveled), 0, 65535).astype(np.uint16)
    Image.fromarray(h_out).save(out_path)
    return h_out


def make_tileable_noise(size: int, sigma: float, seed: int) -> np.ndarray:
    """
    Gera ruído suave perfeitamente repetível (seamless) usando padding circular.
    """
    rng = np.random.default_rng(seed)
    base = rng.integers(0, 256, size=(size, size), dtype=np.uint8)
    pad = max(8, int(math.ceil(sigma * 3.0)))
    padded = np.pad(base, ((pad, pad), (pad, pad)), mode="wrap")
    blurred = Image.fromarray(padded, mode="L").filter(ImageFilter.GaussianBlur(radius=sigma))
    arr = np.array(blurred, dtype=np.float32)[pad:pad + size, pad:pad + size]
    return (arr - arr.mean()) / max(1e-5, arr.std())


def _save_wrapped_strokes(size: int, draw_fn) -> np.ndarray:
    """
    Desenha formas 2D repetindo nas 9 cópias periódicas (3x3) para garantir textura 100% seamless.
    """
    img = Image.new("RGB", (size, size), (0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw_fn(draw, size)
    return np.array(img, dtype=np.float32)


def generate_placeholder_textures(terrain_dir: str):
    """
    Gera 8 texturas placeholder nítidas, distintas e 100% seamless (512x512) correspondentes
    aos 8 canais dos splatmaps (GL_TEXTURE_2D_ARRAY), além das texturas de tronco e copa.
    """
    sz = 512

    # -------------------------------------------------------------------------
    # 0. Asfalto Principal (mat_0_asphalt_main.png): asfalto escuro com brita mineral visível
    # -------------------------------------------------------------------------
    rng = np.random.default_rng(1001)
    n_coarse = make_tileable_noise(sz, 14.0, 1002)
    n_med = make_tileable_noise(sz, 3.0, 1003)
    n_fine = make_tileable_noise(sz, 0.7, 1004)
    # Grãos de brita mineral distintos (1-2px) para deixar claro o aspecto de asfalto
    pebbles = rng.random((sz, sz))
    speck_light = (pebbles > 0.945).astype(np.float32) * 38.0
    speck_dark = (pebbles < 0.060).astype(np.float32) * (-24.0)
    asph_base = 64.0 + n_coarse * 7.0 + n_med * 6.0 + n_fine * 8.0 + speck_light + speck_dark
    asph0 = np.stack([asph_base - 1.0, asph_base, asph_base + 3.0], axis=-1)
    # Sutis microfissuras betuminosas seamless
    img0 = Image.fromarray(np.clip(asph0, 20, 220).astype(np.uint8), mode="RGB")
    d0 = ImageDraw.Draw(img0)
    for k in range(10):
        x, y = int(rng.integers(0, sz)), int(rng.integers(0, sz))
        pts = [(x, y)]
        for _ in range(7):
            x += int(rng.integers(-18, 19))
            y += int(rng.integers(-18, 19))
            pts.append((x, y))
        for ox in (-sz, 0, sz):
            for oy in (-sz, 0, sz):
                d0.line([(px + ox, py + oy) for px, py in pts], fill=(34, 35, 38), width=1)
    img0.save(os.path.join(terrain_dir, "mat_0_asphalt_main.png"))

    # -------------------------------------------------------------------------
    # 1. Asfalto Secundário / Concreto Urbano (mat_1_asphalt_sec.png): cinza desgastado + agregado
    # -------------------------------------------------------------------------
    rng = np.random.default_rng(2001)
    n_c = make_tileable_noise(sz, 16.0, 2002)
    n_m = make_tileable_noise(sz, 3.5, 2003)
    n_f = make_tileable_noise(sz, 0.8, 2004)
    pebbles = rng.random((sz, sz))
    speck = (pebbles > 0.91).astype(np.float32) * 28.0 - (pebbles < 0.07).astype(np.float32) * 20.0
    sec_lum = 104.0 + n_c * 9.0 + n_m * 8.0 + n_f * 9.0 + speck
    asph1 = np.stack([sec_lum + 1.0, sec_lum + 1.0, sec_lum + 2.0], axis=-1)
    Image.fromarray(np.clip(asph1, 30, 235).astype(np.uint8), mode="RGB").save(
        os.path.join(terrain_dir, "mat_1_asphalt_sec.png")
    )

    # -------------------------------------------------------------------------
    # 2. Calçada / Piso Pavimentado (mat_2_sidewalk.png): placas claras de concreto 4x4 (~1m)
    # -------------------------------------------------------------------------
    n_c = make_tileable_noise(sz, 12.0, 3002)
    n_m = make_tileable_noise(sz, 2.5, 3003)
    n_f = make_tileable_noise(sz, 0.8, 3004)
    sw_lum = 176.0 + n_c * 7.0 + n_m * 6.0 + n_f * 6.0
    sw_rgb = np.stack([sw_lum + 3.0, sw_lum, sw_lum - 6.0], axis=-1)
    img_sw = Image.fromarray(np.clip(sw_rgb, 40, 240).astype(np.uint8), mode="RGB")
    d_sw = ImageDraw.Draw(img_sw)
    step = sz // 4  # 128px -> 4x4 lajotas de ~1m cada
    for g in range(0, sz, step):
        # Juntas escuras e bisel claro de calçada
        d_sw.line([(g, 0), (g, sz)], fill=(118, 115, 108), width=3)
        d_sw.line([(g + 3, 0), (g + 3, sz)], fill=(198, 195, 186), width=1)
        d_sw.line([(0, g), (sz, g)], fill=(118, 115, 108), width=3)
        d_sw.line([(0, g + 3), (sz, g + 3)], fill=(198, 195, 186), width=1)
    img_sw.save(os.path.join(terrain_dir, "mat_2_sidewalk.png"))

    # -------------------------------------------------------------------------
    # 3. Grama Aberta (mat_3_grass.png): folhagem/relva verde nítida e variada
    # -------------------------------------------------------------------------
    rng = np.random.default_rng(4001)
    n_c = make_tileable_noise(sz, 18.0, 4002)
    n_m = make_tileable_noise(sz, 4.0, 4003)
    gr = np.zeros((sz, sz, 3), dtype=np.float32)
    gr[:, :, 0] = 66.0 + n_c * 11.0 + n_m * 8.0
    gr[:, :, 1] = 114.0 + n_c * 16.0 + n_m * 11.0
    gr[:, :, 2] = 44.0 + n_c * 8.0 + n_m * 5.0
    img_gr = Image.fromarray(np.clip(gr, 18, 220).astype(np.uint8), mode="RGB")
    d_gr = ImageDraw.Draw(img_gr)
    # Desenha centenas de tufos/folhas de grama seamless
    for _ in range(2800):
        bx = int(rng.integers(0, sz))
        by = int(rng.integers(0, sz))
        dx = int(rng.integers(-5, 6))
        dy = int(rng.integers(-11, -3))
        tone = int(rng.integers(-26, 28))
        col = (
            max(18, min(200, 68 + tone)),
            max(35, min(230, 126 + tone + int(rng.integers(-5, 10)))),
            max(15, min(160, 42 + tone // 2))
        )
        for ox in (-sz, 0, sz):
            for oy in (-sz, 0, sz):
                px, py = bx + ox, by + oy
                if -15 <= px < sz + 15 and -15 <= py < sz + 15:
                    d_gr.line([(px, py), (px + dx, py + dy)], fill=col, width=2)
    img_gr.save(os.path.join(terrain_dir, "mat_3_grass.png"))

    # -------------------------------------------------------------------------
    # 4. Solo de Mata / Serapilheira (mat_4_forest.png): húmus escuro + folhas secas + musgo
    # -------------------------------------------------------------------------
    rng = np.random.default_rng(5001)
    n_c = make_tileable_noise(sz, 16.0, 5002)
    n_m = make_tileable_noise(sz, 3.5, 5003)
    n_f = make_tileable_noise(sz, 1.0, 5004)
    fo = np.zeros((sz, sz, 3), dtype=np.float32)
    fo[:, :, 0] = 56.0 + n_c * 10.0 + n_m * 8.0 + n_f * 7.0
    fo[:, :, 1] = 62.0 + n_c * 11.0 + n_m * 8.0 + n_f * 6.0
    fo[:, :, 2] = 38.0 + n_c * 6.0 + n_m * 5.0 + n_f * 4.0
    img_fo = Image.fromarray(np.clip(fo, 15, 190).astype(np.uint8), mode="RGB")
    d_fo = ImageDraw.Draw(img_fo)
    # Folhas caídas e galhos secos sobre o chão da mata
    leaf_colors = [(105, 78, 44), (88, 64, 38), (64, 84, 42), (122, 92, 50)]
    for _ in range(1600):
        bx = int(rng.integers(0, sz))
        by = int(rng.integers(0, sz))
        rx = int(rng.integers(2, 6))
        ry = int(rng.integers(1, 4))
        col = leaf_colors[int(rng.integers(0, len(leaf_colors)))]
        for ox in (-sz, 0, sz):
            for oy in (-sz, 0, sz):
                px, py = bx + ox, by + oy
                if -10 <= px < sz + 10 and -10 <= py < sz + 10:
                    d_fo.ellipse([px - rx, py - ry, px + rx, py + ry], fill=col)
    img_fo.save(os.path.join(terrain_dir, "mat_4_forest.png"))

    # -------------------------------------------------------------------------
    # 5. Terra Exposta / Barro Vermelho RS (mat_5_dirt.png): argila avermelhada + pedregulhos
    # -------------------------------------------------------------------------
    rng = np.random.default_rng(6001)
    n_c = make_tileable_noise(sz, 15.0, 6002)
    n_m = make_tileable_noise(sz, 4.0, 6003)
    n_f = make_tileable_noise(sz, 1.0, 6004)
    dt = np.zeros((sz, sz, 3), dtype=np.float32)
    dt[:, :, 0] = 148.0 + n_c * 14.0 + n_m * 11.0 + n_f * 8.0
    dt[:, :, 1] =  94.0 + n_c * 10.0 + n_m * 8.0 + n_f * 6.0
    dt[:, :, 2] =  62.0 + n_c *  8.0 + n_m * 6.0 + n_f * 4.0
    img_dt = Image.fromarray(np.clip(dt, 25, 225).astype(np.uint8), mode="RGB")
    d_dt = ImageDraw.Draw(img_dt)
    for _ in range(950):
        bx = int(rng.integers(0, sz))
        by = int(rng.integers(0, sz))
        rad = int(rng.integers(1, 4))
        c = int(rng.integers(85, 195))
        col = (min(220, c + 24), c, max(40, c - 22))
        for ox in (-sz, 0, sz):
            for oy in (-sz, 0, sz):
                px, py = bx + ox, by + oy
                if -8 <= px < sz + 8 and -8 <= py < sz + 8:
                    d_dt.ellipse([px - rad, py - rad, px + rad, py + rad], fill=col)
    img_dt.save(os.path.join(terrain_dir, "mat_5_dirt.png"))

    # -------------------------------------------------------------------------
    # 6. Água Placeholder (mat_6_water.png): textura limpa estática p/ a Lagoa (sem ondas 3D)
    # -------------------------------------------------------------------------
    yy, xx = np.mgrid[0:sz, 0:sz].astype(np.float32)
    tau = 2.0 * math.pi / float(sz)
    rip = (
        np.sin(xx * tau * 4.0 + np.sin(yy * tau * 3.0) * 1.3) * 0.5 +
        np.cos(yy * tau * 6.0 + np.cos(xx * tau * 5.0) * 1.1) * 0.5
    )
    n_w = make_tileable_noise(sz, 14.0, 7002)
    wa = np.zeros((sz, sz, 3), dtype=np.float32)
    wa[:, :, 0] = 44.0 + n_w * 5.0 + rip * 7.0
    wa[:, :, 1] = 88.0 + n_w * 7.0 + rip * 10.0
    wa[:, :, 2] = 108.0 + n_w * 8.0 + rip * 13.0
    Image.fromarray(np.clip(wa, 20, 200).astype(np.uint8), mode="RGB").save(
        os.path.join(terrain_dir, "mat_6_water.png")
    )

    # -------------------------------------------------------------------------
    # 7. Fundação de Prédios (mat_7_building.png): base de concreto estrutural cinza limpo
    # -------------------------------------------------------------------------
    n_c = make_tileable_noise(sz, 18.0, 8002)
    n_f = make_tileable_noise(sz, 1.2, 8003)
    bd_lum = 148.0 + n_c * 6.0 + n_f * 5.0
    bd_rgb = np.stack([bd_lum, bd_lum - 1.0, bd_lum - 3.0], axis=-1)
    img_bd = Image.fromarray(np.clip(bd_rgb, 40, 230).astype(np.uint8), mode="RGB")
    d_bd = ImageDraw.Draw(img_bd)
    bstep = sz // 2  # Lajes grandes de fundação (2x2 por tile = 2m x 2m)
    for g in range(0, sz, bstep):
        d_bd.line([(g, 0), (g, sz)], fill=(104, 102, 99), width=4)
        d_bd.line([(0, g), (sz, g)], fill=(104, 102, 99), width=4)
    img_bd.save(os.path.join(terrain_dir, "mat_7_building.png"))

    # -------------------------------------------------------------------------
    # Árvores Placeholder: Casca de Tronco (tree_bark.png) e Folhagem (tree_foliage.png)
    # -------------------------------------------------------------------------
    rng = np.random.default_rng(9001)
    stripes = rng.normal(0.0, 1.0, size=(sz // 8, sz)).astype(np.float32)
    stripes_img = Image.fromarray(np.clip(128 + stripes * 44, 0, 255).astype(np.uint8), mode="L").resize(
        (sz, sz), resample=Image.Resampling.BICUBIC
    )
    bark_v = (np.array(stripes_img, dtype=np.float32) - 128.0) / 44.0
    bark_fine = make_tileable_noise(sz, 1.0, 9002)
    bark_rgb = np.zeros((sz, sz, 3), dtype=np.float32)
    for c, base in enumerate((88.0, 66.0, 48.0)):
        bark_rgb[:, :, c] = base + bark_v * 16.0 + bark_fine * 6.0
    Image.fromarray(np.clip(bark_rgb, 18, 200).astype(np.uint8), mode="RGB").save(
        os.path.join(terrain_dir, "tree_bark.png")
    )

    # Folhagem placeholder estilizada com clusters distintos de folhas
    fol_bg = np.zeros((sz, sz, 3), dtype=np.float32)
    n_leaf = make_tileable_noise(sz, 8.0, 9003)
    fol_bg[:, :, 0] = 48.0 + n_leaf * 10.0
    fol_bg[:, :, 1] = 92.0 + n_leaf * 14.0
    fol_bg[:, :, 2] = 36.0 + n_leaf * 7.0
    img_fol = Image.fromarray(np.clip(fol_bg, 15, 190).astype(np.uint8), mode="RGB")
    d_fol = ImageDraw.Draw(img_fol)
    leaf_pal = [(42, 84, 32), (56, 108, 40), (74, 128, 48), (62, 118, 44), (36, 74, 28)]
    for _ in range(1800):
        bx = int(rng.integers(0, sz))
        by = int(rng.integers(0, sz))
        rx = int(rng.integers(5, 13))
        ry = int(rng.integers(4, 10))
        col = leaf_pal[int(rng.integers(0, len(leaf_pal)))]
        for ox in (-sz, 0, sz):
            for oy in (-sz, 0, sz):
                px, py = bx + ox, by + oy
                if -20 <= px < sz + 20 and -20 <= py < sz + 20:
                    d_fol.ellipse([px - rx, py - ry, px + rx, py + ry], fill=col)
    img_fol.save(os.path.join(terrain_dir, "tree_foliage.png"))


def generate_placeholder_tree_obj(data_dir: str):
    """
    Gera um modelo 3D low-poly estilizado e bem proporcionado de árvore (`data/tree.obj`
    e `data/tree.mtl`) com dois objetos nomeados (`tree_trunk` e `tree_canopy`).
    """
    mtl_path = os.path.join(data_dir, "tree.mtl")
    obj_path = os.path.join(data_dir, "tree.obj")

    with open(mtl_path, "w", encoding="utf-8") as f:
        f.write("# Placeholder Tree Material - Branquinho Simulator\n")
        f.write("newmtl tree_bark\nKa 0.35 0.26 0.19\nKd 0.42 0.31 0.22\nKs 0.04 0.04 0.04\nNs 8.0\n")
        f.write("map_Kd terrain/tree_bark.png\n\n")
        f.write("newmtl tree_foliage\nKa 0.24 0.40 0.18\nKd 0.30 0.48 0.22\nKs 0.05 0.05 0.05\nNs 12.0\n")
        f.write("map_Kd terrain/tree_foliage.png\n")

    verts, uvs, norms = [], [], []
    trunk_faces, canopy_faces = [], []

    def add_vertex(x, y, z, u, v, nx, ny, nz):
        l = max(1e-6, math.sqrt(nx * nx + ny * ny + nz * nz))
        verts.append((x, y, z))
        uvs.append((u, v))
        norms.append((nx / l, ny / l, nz / l))
        return len(verts)

    n_seg = 8
    trunk_rings = [(-0.35, 0.36, 0.0), (0.80, 0.25, 0.4), (2.85, 0.17, 1.0)]
    ring_indices = []
    for y_val, r_val, v_coord in trunk_rings:
        ring = []
        for i in range(n_seg + 1):
            ang = (2.0 * math.pi * i) / n_seg
            ca, sa = math.cos(ang), math.sin(ang)
            ring.append(add_vertex(r_val * ca, y_val, r_val * sa, float(i) / n_seg, v_coord, ca, 0.12, sa))
        ring_indices.append(ring)

    for r_idx in range(len(ring_indices) - 1):
        r0, r1 = ring_indices[r_idx], ring_indices[r_idx + 1]
        for i in range(n_seg):
            trunk_faces.append((r0[i], r1[i], r1[i + 1]))
            trunk_faces.append((r0[i], r1[i + 1], r0[i + 1]))

    tiers = [
        (2.05, 3.35, 4.75, 1.95, 2.35,  0.00,  0.00, 0.00),
        (3.35, 4.65, 6.10, 1.65, 1.95,  0.12, -0.08, 0.35),
        (4.70, 5.95, 7.35, 1.20, 1.40, -0.08,  0.10, 0.70),
    ]
    n_cseg = 10
    for y_base, y_mid, y_top, r_base, r_mid, xo, zo, phase in tiers:
        cy = 0.5 * (y_base + y_top)
        rings = [
            (y_base, r_base * 0.45, 0.05),
            (y_base + 0.35 * (y_mid - y_base), r_base, 0.30),
            (y_mid, r_mid, 0.58),
            (y_mid + 0.55 * (y_top - y_mid), r_mid * 0.62, 0.82),
            (y_top, 0.04, 0.98)
        ]
        c_rings = []
        for r_y, r_rad, v_c in rings:
            ring = []
            for i in range(n_cseg + 1):
                ang = (2.0 * math.pi * i) / n_cseg + phase
                rr = r_rad * (1.0 + 0.08 * math.sin(3.0 * ang + phase * 2.0))
                px = xo + rr * math.cos(ang)
                pz = zo + rr * math.sin(ang)
                ring.append(add_vertex(px, r_y, pz, float(i) / n_cseg, v_c, px - xo, (r_y - cy) * 0.85 + 0.25, pz - zo))
            c_rings.append(ring)

        for r_idx in range(len(c_rings) - 1):
            r0, r1 = c_rings[r_idx], c_rings[r_idx + 1]
            for i in range(n_cseg):
                canopy_faces.append((r0[i], r1[i], r1[i + 1]))
                canopy_faces.append((r0[i], r1[i + 1], r0[i + 1]))

    with open(obj_path, "w", encoding="utf-8") as f:
        f.write("# Placeholder Tree 3D Model - Branquinho: Rota do Vale\nmtllib tree.mtl\n\n")
        for x, y, z in verts:
            f.write(f"v {x:.4f} {y:.4f} {z:.4f}\n")
        for u, v in uvs:
            f.write(f"vt {u:.4f} {v:.4f}\n")
        for nx, ny, nz in norms:
            f.write(f"vn {nx:.4f} {ny:.4f} {nz:.4f}\n")
        f.write("\no tree_trunk\ng tree_trunk\nusemtl tree_bark\ns 1\n")
        for a, b, c in trunk_faces:
            f.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")
        f.write("\no tree_canopy\ng tree_canopy\nusemtl tree_foliage\ns 1\n")
        for a, b, c in canopy_faces:
            f.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")


def sample_trees_and_export_instances(
    canopy_mask_2k: np.ndarray,
    road_clear_grid: np.ndarray,
    valid_grid: np.ndarray,
    h_uint16: np.ndarray,
    terrain_dir: str
):
    """
    Amostra instâncias 3D de árvores nas áreas de copa detectadas no satélite fora das
    vias (`road_clear_grid`) e fora do spawn inicial `(0, 0)`.
    """
    H, W = h_uint16.shape
    canopy_grid = np.array(
        Image.fromarray(canopy_mask_2k.astype(np.uint8) * 255, mode="L").resize((W, H), resample=Image.Resampling.NEAREST),
        dtype=np.uint8
    ) > 127

    valid_inner = np.array(
        Image.fromarray(valid_grid.astype(np.uint8) * 255, mode="L").filter(ImageFilter.MinFilter(size=9)),
        dtype=np.uint8
    ) > 127

    allowed = canopy_grid & valid_inner & (~road_clear_grid)

    rng = np.random.default_rng(2026)
    cell_step = 11
    trees = []
    half_w, half_h = 0.5 * float(W), 0.5 * float(H)

    for gz in range(8, H - 8, cell_step):
        for gx in range(8, W - 8, cell_step):
            jx = gx + int(rng.integers(-3, 4))
            jz = gz + int(rng.integers(-3, 4))
            if not (4 <= jx < W - 4 and 4 <= jz < H - 4):
                continue
            if not allowed[jz, jx]:
                continue

            wx = float(jx) - half_w
            wz = float(jz) - half_h
            if wx * wx + wz * wz < 16.0 * 16.0:
                continue

            wy = 33.0 + (float(h_uint16[jz, jx]) / 65535.0) * 104.043
            trees.append({
                "x": round(wx, 2),
                "y": round(wy, 2),
                "z": round(wz, 2),
                "scale": round(float(rng.uniform(0.82, 1.32)), 3),
                "yaw": round(float(rng.uniform(0.0, 2.0 * math.pi)), 3),
                "type": int(rng.integers(0, 3))
            })

    with open(os.path.join(terrain_dir, "trees.txt"), "w", encoding="utf-8") as f:
        f.write(f"# {len(trees)} tree instances: world_x world_y world_z scale yaw\n")
        for t in trees:
            f.write(f"{t['x']:.2f} {t['y']:.2f} {t['z']:.2f} {t['scale']:.3f} {t['yaw']:.3f}\n")

    with open(os.path.join(terrain_dir, "instances.json"), "w", encoding="utf-8") as f:
        json.dump({"trees": trees, "buildings": []}, f, indent=2)

    return len(trees)


def main():
    parser = argparse.ArgumentParser(description="Gerador de Terreno e Instâncias - Branquinho: Rota do Vale")
    parser.add_argument("--sat", default="tools/bruto_2012_mascara.tiff", help="Imagem .tif/.tiff já mascarada")
    parser.add_argument("--heightmap", default="data/vale.png", help="Heightmap 16-bit de entrada")
    parser.add_argument("--outdir", default="data/terrain", help="Diretório de saída dos ativos de terreno")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    data_dir = os.path.dirname(args.heightmap) or "data"

    # Remove texturas antigas de detalhe se existirem para manter o diretório limpo
    for old_name in ("detail_asphalt.png", "detail_sidewalk.png", "detail_grass.png", "detail_dirt.png", "detail_water.png"):
        old_p = os.path.join(args.outdir, old_name)
        if os.path.exists(old_p):
            os.remove(old_p)

    # 1. Reprojeta o .tiff mascarado para 4096x4096 alinhado com vale.png
    sat_4k = load_and_warp_satellite(args.sat, 4096, 4096)
    pil_4k = Image.fromarray(sat_4k, mode="RGB")
    sat_2k = np.array(pil_4k.resize((2048, 2048), resample=Image.Resampling.BILINEAR), dtype=np.uint8)
    sat_grid = np.array(pil_4k.resize((1930, 1703), resample=Image.Resampling.BILINEAR), dtype=np.uint8)

    # 2. Máscara válida sem buracos internos de sombras da mata
    print("[2/6] Extraindo máscara válida (removendo áreas pretas da máscara)...")
    valid_grid = extract_clean_valid_mask(sat_grid)
    Image.fromarray(valid_grid.astype(np.uint8) * 255, mode="L").save(os.path.join(args.outdir, "valid_mask.png"))
    valid_2k = np.array(
        Image.fromarray(valid_grid.astype(np.uint8) * 255, mode="L").resize((2048, 2048), resample=Image.Resampling.BILINEAR),
        dtype=np.uint8
    ) > 127
    print(f"      -> Área útil ativa: {100.0 * float(np.mean(valid_grid)):.1f}%.")

    # 3. Extração viária pura por Visão Computacional sobre o satélite (zero OSM)
    print("[3/6] Detectando pistas diretamente do satélite + desobstrução sob copas...")
    cv_main_grid, cv_sec_grid, cv_bridge_grid, road_cv_grid, se_path = extract_satellite_road_masks(sat_grid, valid_grid)
    print(f"      -> Pixels viários detectados: main={int(cv_main_grid.sum())}, sec={int(cv_sec_grid.sum())}, pontes_copa={int(cv_bridge_grid.sum())}.")

    def _upscale_mask_2k(mask_bool: np.ndarray, blur_r: float = 0.8) -> np.ndarray:
        u8 = mask_bool.astype(np.uint8) * 255
        im = Image.fromarray(u8, mode="L").resize((2048, 2048), resample=Image.Resampling.BILINEAR)
        if blur_r > 0.0:
            im = im.filter(ImageFilter.GaussianBlur(radius=blur_r))
        return np.array(im, dtype=np.float32) / 255.0

    main_road_2k = _upscale_mask_2k(cv_main_grid, blur_r=0.8)
    sec_road_2k = _upscale_mask_2k(cv_sec_grid, blur_r=0.8)
    hidden_road_2k = _upscale_mask_2k(cv_bridge_grid, blur_r=1.0)

    road_bin_2k_u8 = (np.clip(main_road_2k + sec_road_2k, 0.0, 1.0) > 0.35).astype(np.uint8) * 255
    dilated_edge = np.array(Image.fromarray(road_bin_2k_u8, mode="L").filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(radius=0.8)), dtype=np.float32) / 255.0
    edge_ring_2k = np.clip(dilated_edge - np.clip(main_road_2k + sec_road_2k, 0.0, 1.0), 0.0, 1.0)

    snapped_corridors = snap_corridors_dijkstra(sat_grid, valid_grid, road_cv_grid=road_cv_grid, se_path_override=se_path)
    road_clear_grid = np.array(Image.fromarray(road_cv_grid.astype(np.uint8) * 255, mode="L").filter(ImageFilter.MaxFilter(9)), dtype=np.uint8) > 127

    # 4. Splatmaps (8 canais) + Macro Albedo 4K
    print("[4/6] Classificando 8 materiais -> splatmap_0.png, splatmap_1.png e macro_albedo.png...")
    splat0, splat1, unocc_2k, water_bin_2k, bldg_bin_2k, canopy_mask_2k = classify_terrain_and_build_splatmaps(
        sat_2k, valid_2k, main_road_2k, sec_road_2k, hidden_road_2k, edge_ring_2k
    )
    Image.fromarray(splat0, mode="RGBA").save(os.path.join(args.outdir, "splatmap_0.png"))
    Image.fromarray(splat1, mode="RGBA").save(os.path.join(args.outdir, "splatmap_1.png"))

    macro_4k = build_macro_albedo_4k(sat_4k, valid_2k, unocc_2k, bldg_bin_2k)
    Image.fromarray(macro_4k, mode="RGB").save(os.path.join(args.outdir, "macro_albedo.png"))

    # 5. Nivelamento de pistas e lagoa no heightmap 16-bit
    vale_proc_path = os.path.join(data_dir, "vale_processed.png")
    print(f"[5/6] Nivelando pistas em '{vale_proc_path}' (16-bit)...")
    h_uint16 = process_heightmap_16bit(args.heightmap, snapped_corridors, water_bin_2k, vale_proc_path)

    # 6. 8 Texturas placeholder seamless + árvore 3D + instâncias
    print("[6/6] Gerando as 8 texturas placeholder (mat_0..mat_7), 'data/tree.obj' e instâncias...")
    generate_placeholder_textures(args.outdir)
    generate_placeholder_tree_obj(data_dir)
    num_trees = sample_trees_and_export_instances(canopy_mask_2k, road_clear_grid, valid_grid, h_uint16, args.outdir)
    print(f"Concluído! {num_trees} árvores instanciadas fora das vias.")


if __name__ == "__main__":
    main()
