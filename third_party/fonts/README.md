# third_party/fonts — Noto Sans SC 子集字体

GUI 中文界面字体（中英双语共用，一套字体渲染）。

## 文件

| 文件 | 说明 |
|---|---|
| `NotoSansSC-subset.ttf` | 子集字体（Bold wght=700，GB2312 全量 6763 汉字 + ASCII + 常用标点 + `· → ← ↑ ↓ × ±`，约 2.2MB），编译期经 `xxd -i` 嵌入为 `gui/font_data.h` |
| `chars.txt` | 子集化的字符表（可复现子集化） |
| `OFL.txt` | 上游字体许可证（SIL OFL 1.1，允许再分发） |

## 来源

- 上游：Google Fonts 官方仓库（可变字体 `NotoSansSC[wght].ttf`，wght 轴 100~900，OFL 1.1）
  `https://github.com/google/fonts/tree/main/ofl/notosanssc`

## 复现命令

> ⚠️ 注意：`instantiateVariableFont` 默认返回新对象（非 inplace），
> **必须用 `inplace=True` 或接收返回值**，否则保存的是未实例化的 Thin(100) 原始字体（曾因此加粗未生效）。

```bash
# 1. 下载官方可变字体
curl -sSL -o "NotoSansSC[wght].ttf" \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"

# 2. 实例化为静态 Bold(wght=700, inplace=True 是关键)
python -c "
from fontTools import ttLib
from fontTools.varLib.instancer import instantiateVariableFont
f = ttLib.TTFont('NotoSansSC[wght].ttf')
instantiateVariableFont(f, {'wght': 700}, inplace=True)
f['OS/2'].usWeightClass = 700
f.save('NotoSansSC-Bold.ttf')
"

# 3. 按字符表子集化（chars.txt = ASCII + GB2312 全量 + 常用标点）
python -m fontTools.subset NotoSansSC-Bold.ttf \
  --text-file=chars.txt --unicodes=U+00B7,U+00D7,U+00B1,U+2190-2193 \
  --output-file=NotoSansSC-subset.ttf

# 4. 生成 C 数组头（编译期嵌入）
xxd -i NotoSansSC-subset.ttf > ../../gui/font_data.h
```

依赖：`pip install fonttools`（仅开发期，构建不依赖）。
