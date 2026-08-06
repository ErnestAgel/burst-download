# third_party/fonts — Noto Sans SC 子集字体

GUI 中文界面字体（中英双语共用，一套字体渲染）。

## 文件

| 文件 | 说明 |
|---|---|
| `NotoSansSC-subset.ttf` | 子集字体（GB2312 全量 6763 汉字 + ASCII + 常用标点，约 3.6MB），编译期经 `xxd -i` 嵌入为 `gui/font_data.h` |
| `chars.txt` | 子集化的字符表（可复现子集化） |
| `OFL.txt` | 上游字体许可证（SIL OFL 1.1，允许再分发） |

## 来源

- 上游：Google Fonts 官方仓库（可变字体 `NotoSansSC[wght].ttf`，OFL 1.1）
  `https://github.com/google/fonts/tree/main/ofl/notosanssc`

## 复现命令

```bash
# 1. 下载官方可变字体
curl -sSL -o "NotoSansSC[wght].ttf" \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"

# 2. 实例化为静态 Regular(wght=400)
python -c "
from fontTools import ttLib
from fontTools.varLib.instancer import instantiateVariableFont
f = ttLib.TTFont('NotoSansSC[wght].ttf')
instantiateVariableFont(f, {'wght': 400})
f.save('NotoSansSC-Regular.ttf')
"

# 3. 按字符表子集化（chars.txt = ASCII + GB2312 全量 + 常用标点）
python -m fontTools.subset NotoSansSC-Regular.ttf \
  --text-file=chars.txt --output-file=NotoSansSC-subset.ttf

# 4. 生成 C 数组头（编译期嵌入）
xxd -i NotoSansSC-subset.ttf > ../../gui/font_data.h
```

依赖：`pip install fonttools`（仅开发期，构建不依赖）。
