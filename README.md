# 🗣️ Win7-TTS — Free Offline Persian Text-to-Speech for Windows 7

**Convert Persian text to natural speech instantly. No internet. No setup. Just download and use.**

> A lightweight, self-contained text-to-speech tool for Windows 7 that works completely offline. Built for Persian, works with any language supported by Piper/VITS models.

---

## 📢 Choose Your Language | انتخاب زبان

- **[English](#-get-started-in-30-seconds)** ← You are here
- **[فارسی (Farsi)](#-شروع-سریع-در-۳۰-ثانیه)**

---

## 💬 Join Us

**[📱 Telegram Channel](https://t.me/persian_tts)** — Get updates, ask questions, share your creations

---

# 🌐 ENGLISH VERSION

## ⚡ Get Started in 30 Seconds

### 1. Download
👉 **[Download the latest release (ZIP)](https://github.com/karim23657/offline-persian-tts-win7/releases/latest)**

### 2. Extract
Unzip anywhere on your computer.

### 3. Run
Choose your preferred method:

**Option A: Web Interface (Easiest)** 🌐
```bat
scripts\server.cmd
```
Then open your browser to: **http://127.0.0.1:8756/**
- Beautiful web interface
- Works on any device on your network
- No installation needed

**Option B: GUI Application** 🖥️
```bat
scripts\gui.cmd
```
- Graphical interface (native Windows)
- Fastest performance

**Option C: Command Line** ⚡
```bat
scripts\say.cmd "سلام دنیا"
```
- Fastest, no GUI overhead

**That's it.** No Python. No installation. No internet needed. Speech appears in `out.wav`.

---

## 🎯 What Can You Do?

| Task | Command |
|------|---------|
| **Start Web Server** | `scripts\server.cmd` → Open http://127.0.0.1:8756/ |
| **GUI Interface** | `scripts\gui.cmd` |
| **Speak text** | `scripts\say.cmd "سلام دنیا"` |
| **Save as custom file** | `scripts\say.cmd "text" output.wav` |
| **Convert Persian text file** | `scripts\say.cmd --file input.txt` |
| **Batch process (one wav per line)** | `scripts\say_batch.cmd lines.txt` |
| **Download other voices** | `scripts\get_model.cmd` |
| **Troubleshoot problems** | `scripts\diagnose.cmd` |

---

## ✨ Why This Tool?

✅ **Works on Windows 7** — the only offline TTS for old Windows  
✅ **Completely offline** — no internet, no API calls, no tracking  
��� **Persian-first** — supports Persian beautifully (UTF-8)  
✅ **Web Interface** — access from any browser (http://127.0.0.1:8756)  
✅ **Multilingual** — 10+ Persian voices + models for other languages  
✅ **Lightweight** — fits in a USB stick  
✅ **Free and open source** — no ads, no subscriptions  

---

## 🌐 Web Server Mode (NEW!)

The easiest way to use Win7-TTS:

```bat
scripts\server.cmd
```

1. Run the command above
2. Your browser opens automatically to: **http://127.0.0.1:8756/**
3. Type or paste Persian text
4. Click **Generate**
5. Hear the speech and download the `.wav`

**Features:**
- 📱 Works on phone, tablet, or any computer with a browser
- 🔗 Share the URL with others on your network
- 🎨 Beautiful, responsive interface
- 🚀 No plugins, no installation
- 🔐 Completely local — nothing leaves your machine

**Access from:**
- Same computer: `http://localhost:8756` or `http://127.0.0.1:8756`
- Another computer on the network: `http://<your-ip>:8756` (find your IP with `ipconfig`)

---

## 🎤 Available Voices & Models

### Quick Download Links

| Voice | Language | Download | Size | Quality |
|-------|----------|----------|------|---------|
| **`gyro`** ⭐ | Persian | [Included] | 450 MB | Best |
| `amir` | Persian | [Run `get_model.cmd`] | 400 MB | Excellent |
| `reza` | Persian + English | [Run `get_model.cmd`] | 380 MB | Very Good |
| `haaniye` | Persian | [Run `get_model.cmd`] | 100 MB | Good |
| `ganji` | Persian | [Run `get_model.cmd`] | 150 MB | Good |
| `ganji-adabi` | Persian | [Run `get_model.cmd`] | 160 MB | Very Good |
| `mms` | 1000+ languages | [Run `get_model.cmd`] | 600 MB | Good |
| `negoo` | Persian (Female) | [Run `get_model.cmd`] | 200 MB | Good |
| `arash` | Persian (Male) | [Run `get_model.cmd`] | 200 MB | Good |
| `keyan` | Persian (Male) | [Run `get_model.cmd`] | 200 MB | Good |
| `matab` | Persian (Female) | [Run `get_model.cmd`] | 200 MB | Good |
| `shiva` | Persian (Female) | [Run `get_model.cmd`] | 200 MB | Good |
| `bahman` | Persian (Male) | [Run `get_model.cmd`] | 200 MB | Good |

**How to download models:**
```bat
# Interactive menu (recommended)
scripts\get_model.cmd

# View all available models
scripts\get_model.cmd list

# Or download from the web interface
- Open http://127.0.0.1:8756/
- Use model selector to download
```

**Manual Download:**
All models are hosted on [HuggingFace](https://huggingface.co/karim23657). Browse and download directly if you prefer.

---

## 📖 First Time? Read This

### Web Interface Method (Recommended for Beginners) 🌐

1. Extract the ZIP
2. Double-click `scripts\server.cmd`
3. Browser opens automatically
4. Type or paste Persian text
5. Click **Generate**
6. Hear the speech play back in the browser
7. Click **Download** to save the `.wav` file

### GUI Method (Windows Application) 🖥️

1. Extract the ZIP
2. Double-click `scripts\gui.cmd`
3. Type or paste Persian text
4. Click **Generate**
5. Hear the speech play back
6. Click **Open folder** to find `out.wav`

### Command Line Method ⚡

```bat
# Simple: just speak
scripts\say.cmd "سلام"

# Save to a file
scripts\say.cmd "سلام دنیا" output.wav

# From a UTF-8 text file (BEST for Persian)
scripts\say.cmd --file input.txt output.wav

# Change speed
scripts\say.cmd --file input.txt output.wav --speed 1.5

# One wav file per line of text
scripts\say_batch.cmd lines.txt
```

**Always use `--file` for Persian** — it handles special characters correctly.

---

## ❓ Frequently Asked Questions

### Q: What's the easiest way to use this?
**A:** Use the web server:
```bat
scripts\server.cmd
```
Then open http://127.0.0.1:8756 in your browser. No installation needed!

### Q: Can I access the web interface from my phone?
**A:** Yes! Find your computer's IP (run `ipconfig` in Command Prompt), then visit `http://<your-ip>:8756` from your phone. Both must be on the same network.

### Q: Why is it just question marks "????"?
**A:** You're using the wrong method. Always use:
```bat
scripts\say.cmd --file yourfile.txt
```
Make sure your text file is saved as **UTF-8** (Notepad: Save As → Encoding: UTF-8).

### Q: I see "Failed to create DXGI factory"
**A:** That's normal. The engine looks for a graphics card. You don't have one (or don't need one), and that's fine. Keep going.

### Q: The audio is just silence or very quiet
**A:** Your CPU might be old. Run this to check:
```bat
scripts\diagnose.cmd
```
If it says `AVX: NO`, your CPU is too old (pre-2011).

### Q: Can I use this on Windows 10/11?
**A:** Yes, it works on modern Windows too. But it's optimized for Windows 7.

### Q: How do I change the voice speed?
**A:**
```bat
scripts\say.cmd --file text.txt output.wav --speed 1.5
```
- `1.0` = normal
- `1.5` = 50% faster
- `0.8` = 20% slower

### Q: How do I download additional models/voices?
**A:** Run this command:
```bat
scripts\get_model.cmd
```
Then choose the voice you want from the menu. It will download automatically to the `models\` folder.

Or use the web interface:
- Go to http://127.0.0.1:8756/
- Click the model selector
- Choose and download

---

## 🔧 Troubleshooting

### "The procedure entry point could not be located"
→ The `runtime\` folder is incomplete. **Re-extract the ZIP from the release.**

### "VCRUNTIME140.dll was not found"
→ Install the [Visual C++ 2015-2022 Redistributable (x64)](https://support.microsoft.com/en-us/help/2977003)

### "could not create the TTS engine"
→ The model folder is missing or has the wrong path. Run:
```bat
scripts\diagnose.cmd
```

### Server won't start / "Port 8756 already in use"
→ Another application is using port 8756. Either:
1. Close the other application
2. Edit `scripts\server.cmd` and change `8756` to a different port (e.g., `8757`)
3. Stop the server with `Ctrl+C` in the Command Prompt

### Speech is very slow / CPU is maxed
→ VITS models are CPU-intensive. Try:
```bat
scripts\say.cmd --file text.txt output.wav --threads 2
```
Or switch to the smaller `ganji` model (16 kHz).

### Still stuck?
→ Run the diagnostic:
```bat
scripts\diagnose.cmd
```
Copy the **entire output** and open an issue on GitHub or ask in our [Telegram channel](https://t.me/persian_tts).

---

## 📁 What's Inside?

```
win7-tts\
  ├─ README.md                     ← you are here
  ├─ scripts\
  │  ├─ server.cmd                 ← start web interface (NEW!)
  │  ├─ gui.cmd                    ← visual interface
  │  ├─ say.cmd                    ← command line tool
  │  ├─ say_batch.cmd              ← batch processor
  │  ├─ get_model.cmd              ← download voices
  │  ├─ diagnose.cmd               ← troubleshoot
  │  └─ gui.py                     ← Python GUI (optional)
  ├─ runtime\                      ← the engine (don't move files around)
  │  ├─ sherpa-onnx-offline-tts.exe
  │  ├─ say.exe
  │  ├─ tts_gui.exe
  │  ├─ tts_server.exe             ← web server executable
  │  ├─ w7shim.dll                 ← Windows 7 compatibility
  │  └─ *.dll                      ← libraries
  ├─ models\
  │  └─ vits-piper-fa_IR-gyro-medium\  ← default voice (already included)
  ├─ docs\                         ← technical documentation
  └─ out.wav                       ← where your audio files are saved
```

---

## 🚀 Advanced Options

### Use Different Voice
Edit the `MODEL_DIR` line in `scripts\say.cmd`:
```bat
set "MODEL_DIR=%CD%\models\vits-piper-fa_IR-amir-medium"
```

### Run Server on Custom Port
Edit `scripts\server.cmd`:
```bat
runtime\tts_server.exe --port 9000
```

### Drive the Engine Directly
For experts only:
```bat
runtime\sherpa-onnx-offline-tts.exe ^
  --vits-model=models\gyro\fa_IR-gyro-medium.onnx ^
  --vits-tokens=models\gyro\tokens.txt ^
  --vits-data-dir=models\gyro\espeak-ng-data ^
  --output-filename=out.wav "hello"
```

### Batch Processing
Generate one `.wav` per line of a text file:
```bat
scripts\say_batch.cmd myfile.txt
```
Output goes to `lines\` folder (one file per line).

---

## 🛠️ For Developers

Want to understand how it works or contribute?

- **[How it works](docs/how_to.md)** — architecture and design decisions
- **[Compiling](docs/compiling.md)** — build from source
- **[Windows 7 fixes](docs/win7.md)** — the technical challenges and solutions
- **[Upgrading sherpa-onnx](docs/upgrading-sherpa.md)** — update the engine
- **[Publishing](docs/github-upload.md)** — release process

---

## 🎓 Key Features Explained

### ✅ Works Offline
No internet connection needed. The model and engine are bundled. This is a **huge** advantage over web APIs like Google TTS or Azure Speech.

### ✅ No Installation
Just extract and run. No Python. No admin rights. No registry changes. Perfect for USB sticks or locked-down machines.

### ✅ Web Interface
Access from any browser — desktop, phone, tablet. Share across your network. No plugins or extensions needed.

### ✅ Handles Persian Correctly
Persian text needs **UTF-8 encoding**. Most tools fail here. We handle it automatically — Persian text will sound correct.

### ✅ Multiple Voices & Languages
Start with Persian (`gyro` voice). Download more via `scripts\get_model.cmd`. We have 10+ Persian voices and multilingual models.

### ✅ Small Download
Only 200-500 MB depending on which models you add. Small enough for USB.

---

## ❗ Common Issues at a Glance

| Problem | Solution |
|---------|----------|
| "????" instead of Persian | Use `--file` with UTF-8 text file |
| No audio / silence | CPU too old? Run `scripts\diagnose.cmd` |
| DLL not found errors | Reinstall Visual C++ 2015-2022 Redistributable (x64) |
| "procedure entry point" error | Re-extract the ZIP file |
| Server won't start | Port 8756 in use. Change port in `server.cmd` |
| Very slow synthesis | Lower `--threads`, or use smaller model |

---

## 📊 Performance

| Model | Speed | Quality | Size |
|-------|-------|---------|------|
| gyro (default) | Fast | Best | 450 MB |
| ganji | Fastest | Good | 150 MB |
| haaniye | Fast | Good | 100 MB |
| mms | Medium | Good | 600 MB |

On a modern CPU (2011+), expect **real-time or faster** (speech takes 3 seconds, synthesis takes 2-3 seconds).

---

## 📜 License & Credits

- **sherpa-onnx** (k2-fsa) — Apache-2.0
- **ONNX Runtime** (Microsoft) — MIT
- **Piper / VITS models** — individual licenses (see each model)
- **This tool** — MIT

Persian models by [karim23657](https://huggingface.co/karim23657)

---

## 🤝 Need Help?

1. **Read the FAQ above** — most questions are answered
2. **Run the diagnostic** — `scripts\diagnose.cmd` and paste the output
3. **Join our Telegram** — **[📱 Telegram Channel](https://t.me/persian_tts)** 
4. **Check the docs** — see `docs/` folder for technical details
5. **Open an issue** — [GitHub Issues](https://github.com/karim23657/offline-persian-tts-win7/issues)

---

## 🌍 Use Cases

✅ Create audiobooks in Persian  
✅ Generate voiceovers for videos  
✅ Automate speech for accessibility tools  
✅ Build chatbots with speech output  
✅ Test pronunciation  
✅ Learn Persian pronunciation  
✅ Use on old machines that can't run modern software  
✅ Web-based interface for shared use across a network  

---

## What Makes This Different?

| Feature | This Tool | Google TTS | Azure Speech | Piper |
|---------|-----------|-----------|--------------|-------|
| **Offline** | ✅ | ❌ | ❌ | ✅ |
| **Windows 7** | ✅ | ❌ | ❌ | ❌ |
| **Persian** | ✅ | ✅ | ✅ | ❌ |
| **Free** | ✅ | ❌ | ❌ | ✅ |
| **GUI** | ✅ | N/A | N/A | ❌ |
| **Web Interface** | ✅ | N/A | N/A | ❌ |
| **No setup** | ✅ | N/A | ❌ | ❌ |

---

**🎉 Ready? [Download the latest release now](https://github.com/karim23657/offline-persian-tts-win7/releases/latest)**

Extract → Run `scripts\server.cmd` → Open browser → Type Persian → Hear speech. Done!

---

---

---

# 🌐 نسخه فارسی

# 🗣️ Win7-TTS — متن به گفتار فارسی آفلاین برای ویندوز 7

**متن فارسی را به گفتار طبیعی تبدیل کنید. بدون اینترنت. بدون نصب. فقط دانلود و استفاده کنید.**

---

## 💬 به ما بپیوندید

**[📱 کانال تلگرام](https://t.me/persian_tts)** — به‌روزرسانی‌ها، پاسخ سؤالات، اشتراک‌گذاری آفرینش‌ها

---

## ⚡ شروع سریع در ۳۰ ثانیه

### 1. دانلود
👉 **[دانلود آخرین نسخه (ZIP)](https://github.com/karim23657/offline-persian-tts-win7/releases/latest)**

### 2. استخراج فایل
فای�� ZIP را در هر جای کامپیوتر خود استخراج کنید.

### 3. اجرا
یکی از این روش‌ها را انتخاب کنید:

**گزینه الف: وب‌سایت (ساده‌ترین)** 🌐
```bat
scripts\server.cmd
```
سپس در مرورگر خود باز کنید: **http://127.0.0.1:8756/**
- رابط وب زیبا
- از هر دستگاهی در شبکه کار می‌کند
- نیاز به نصب ندارد

**گزینه ب: برنامه GUI** 🖥️
```bat
scripts\gui.cmd
```
- رابط گرافیکی (ویندوز بومی)
- سریع‌ترین کارایی

**گزینه ج: خط فرمان** ⚡
```bat
scripts\say.cmd "سلام دنیا"
```
- سریع‌ترین، بدون GUI

**همین است.** بدون Python. بدون نصب. بدون اینترنت. فایل صوتی در `out.wav` ذخیره می‌شود.

---

## 🎯 کار‌های ممکن

| کار | دستور |
|-----|--------|
| **شروع سرور وب** | `scripts\server.cmd` → باز کردن http://127.0.0.1:8756/ |
| **رابط GUI** | `scripts\gui.cmd` |
| **گفتن متن** | `scripts\say.cmd "سلام دنیا"` |
| **ذخیره در فایل سفارشی** | `scripts\say.cmd "متن" output.wav` |
| **تبدیل فایل متنی فارسی** | `scripts\say.cmd --file input.txt` |
| **پردازش دسته‌ای** | `scripts\say_batch.cmd lines.txt` |
| **دانلود صدای دیگر** | `scripts\get_model.cmd` |
| **رفع مشکل** | `scripts\diagnose.cmd` |

---

## ✨ چرا این ابزار؟

✅ **برای ویندوز 7** — تنها ابزار متن به گفتار آفلاین برای ویندوز قدیم  
✅ **کاملاً آفلاین** — بدون اینترنت، بدون ردگیری  
✅ **بهینه برای فارسی** — از UTF-8 به‌درستی پشتیبانی می‌کند  
✅ **رابط وب** — از هر مرورگری دسترسی (http://127.0.0.1:8756)  
✅ **چند صدای فارسی** — بیش از ۱۰ صدای مختلف فارسی  
✅ **سبک و کوچک** — در یک فلش مموری جا می‌گیرد  
✅ **رایگان و متن‌باز** — بدون تبلیغات و اشتراک  

---

## 🌐 حالت سرور وب (جدید!)

ساده‌ترین راه برای استفاده از Win7-TTS:

```bat
scripts\server.cmd
```

1. دستور بالا را اجرا کنید
2. مرورگر شما خودکار باز می‌شود: **http://127.0.0.1:8756/**
3. متن فارسی را تایپ یا چسباندن کنید
4. روی **Generate** کلیک کنید
5. صدا پخش می‌شود و می‌توانید دانلود کنید

**ویژگی‌ها:**
- 📱 از گوشی، تبلت یا هر کامپیوتری با مرورگر کار می‌کند
- 🔗 آدرس را با دیگران در شبکه به‌اشتراک بگذارید
- 🎨 رابط زیبا و پاسخ‌گو
- 🚀 بدون پلاگین، بدون نصب
- 🔐 کاملاً محلی — هیچ اطلاعاتی به بیرون نمی‌رود

**دسترسی از:**
- همان کامپیوتر: `http://localhost:8756` یا `http://127.0.0.1:8756`
- کامپیوتر دیگری در شبکه: `http://<your-ip>:8756` (IP خود را با `ipconfig` پیدا کنید)

---

## 🎤 صداها و مدل‌های دانلودی

| صدا | زبان | دانلود | حجم | کیفیت |
|-----|------|--------|------|-------|
| **`gyro`** ⭐ | فارسی | [موجود است] | 450 MB | بهترین |
| `amir` | فارسی | [اجرای `get_model.cmd`] | 400 MB | عالی |
| `reza` | فارسی + انگلیسی | [اجرای `get_model.cmd`] | 380 MB | خوب جداً |
| `haaniye` | فارسی | [اجرای `get_model.cmd`] | 100 MB | خوب |
| `ganji` | فارسی | [اجرای `get_model.cmd`] | 150 MB | خوب |
| `ganji-adabi` | فارسی (ادبی) | [اجرای `get_model.cmd`] | 160 MB | خوب جداً |
| `mms` | ۱۰۰۰+ زبان | [اجرای `get_model.cmd`] | 600 MB | خوب |
| `negoo` | فارسی (زنانه) | [اجرای `get_model.cmd`] | 200 MB | خوب |
| `arash` | فارسی (مردانه) | [اجرای `get_model.cmd`] | 200 MB | خوب |
| `keyan` | فارسی (مردانه) | [اجرای `get_model.cmd`] | 200 MB | خوب |
| `matab` | فارسی (زنانه) | [اجرای `get_model.cmd`] | 200 MB | خوب |
| `shiva` | فارسی (زنانه) | [اجرای `get_model.cmd`] | 200 MB | خوب |
| `bahman` | فارسی (مردانه) | [اجرای `get_model.cmd`] | 200 MB | خوب |

**چگونه صدای دیگری دانلود کنید:**
```bat
# منوی تعاملی (توصیه می‌شود)
scripts\get_model.cmd

# دیدن تمام مدل‌های دستیاب
scripts\get_model.cmd list

# یا از رابط وب استفاده کنید
http://127.0.0.1:8756/
```

---

## 📖 اولین بار؟ این را بخوانید

### روش رابط وب (توصیه شده برای مبتدیان) 🌐

1. فایل ZIP را استخراج کنید
2. روی `scripts\server.cmd` دوبار کلیک کنید
3. مرورگر خودکار باز می‌شود
4. متن فارسی را تایپ کنید
5. روی **Generate** کلیک کنید
6. صدا در مرورگر پخش می‌شود
7. روی **Download** کلیک کنید تا فایل `.wav` ذخیره شود

### روش GUI (برنامه ویندوز) 🖥️

1. فایل ZIP را استخراج کنید
2. روی `scripts\gui.cmd` دوبار کلیک کنید
3. متن فارسی را تایپ کنید
4. روی **Generate** کلیک کنید
5. صدا پخش می‌شود
6. برای یافتن `out.wav` روی **Open folder** کلیک کنید

### روش خط فرمان ⚡

```bat
# ساده: فقط گفتن
scripts\say.cmd "سلام"

# ذخیره در فایل
scripts\say.cmd "سلام دنیا" output.wav

# از یک فایل متنی UTF-8 (بهترین برای فارسی)
scripts\say.cmd --file input.txt output.wav

# تغییر سرعت
scripts\say.cmd --file input.txt output.wav --speed 1.5

# یک فایل صوتی برای هر سطر
scripts\say_batch.cmd lines.txt
```

**برای فارسی، همیشه `--file` استفاده کنید** — حروف خاص را به‌درستی کنترل می‌کند.

---

## ❓ سوالات متداول

### س: ساده‌ترین روش استفاده چیست؟
**ج:** از سرور وب استفاده کنید:
```bat
scripts\server.cmd
```
سپس http://127.0.0.1:8756 را در مرورگر باز کنید. نیاز به نصب ندارد!

### س: می‌توانم از رابط وب از روی گوشی‌ام دسترسی پیدا کنم؟
**ج:** بله! IP کامپیوتر خود را پیدا کنید (`ipconfig`), سپس از گوشی خود `http://<ip>:8756` را باز کنید. هر دو باید در یک شبکه باشند.

### س: چرا به جای متن فارسی "????" نشان می‌دهد؟
**ج:** شما از روش غلط استفاده می‌کنید. همیشه استفاده کنید:
```bat
scripts\say.cmd --file yourfile.txt
```
اطمینان حاصل کنید فایل متنی شما **UTF-8** است (Notepad: Save As → Encoding: UTF-8).

### س: پیام "Failed to create DXGI factory" دیدم
**ج:** این طبیعی است. موتور به دنبال کارت گرافیک می‌گردد. مشکلی نیست.

### س: صدا سکوت یا بسیار ضعیف است
**ج:** CPU شما ممکن است قدیم باشد. این را اجرا کنید:
```bat
scripts\diagnose.cmd
```
اگر "AVX: NO" نوشت، CPU شما خیلی قدیم است (قبل از 2011).

### س: می‌توانم از این در ویندوز 10/11 استفاده کنم؟
**ج:** بله، در ویندوز مدرن هم کار می‌کند. اما برای ویندوز 7 بهینه است.

### س: چگونه سرعت صدا را تغییر دهم؟
**ج:**
```bat
scripts\say.cmd --file text.txt output.wav --speed 1.5
```
- `1.0` = معمولی
- `1.5` = 50% سریع‌تر
- `0.8` = 20% آهسته‌تر

### س: چگونه صدای بیشتری دانلود کنم؟
**ج:** این دستور را اجرا کنید:
```bat
scripts\get_model.cmd
```

یا از رابط وب استفاده کنید:
- http://127.0.0.1:8756/ را باز کنید
- انتخاب‌گر مدل را کلیک کنید
- صدا را انتخاب و دانلود کنید

---

## 🔧 رفع مشکل

### "The procedure entry point could not be located"
→ پوشه `runtime\` ناقص است. **ZIP را دوباره استخراج کنید.**

### "VCRUNTIME140.dll was not found"
→ [Visual C++ 2015-2022 Redistributable (x64)](https://support.microsoft.com/en-us/help/2977003) را نصب کنید.

### "could not create the TTS engine"
→ مسیر مدل اشتباه است. اجرا کنید:
```bat
scripts\diagnose.cmd
```

### سرور راه‌اندازی نمی‌شود / "Port 8756 already in use"
→ یک برنامه دیگر از پورت 8756 استفاده می‌کند. یا:
1. برنامه دیگر را ببندید
2. `scripts\server.cmd` را ویرایش کنید و `8756` را به پورت دیگر تغییر دهید (مثلاً `8757`)
3. سرور را با `Ctrl+C` متوقف کنید

### سنتز صدا بسیار آهسته است
→ VITS CPU-intensive است. تلاش کنید:
```bat
scripts\say.cmd --file text.txt output.wav --threads 2
```

---

## 📁 محتوی پوشه

```
win7-tts\
  ├─ README.md                     ← این فایل
  ├─ scripts\
  │  ├─ server.cmd                 ← شروع رابط وب (جدید!)
  │  ├─ gui.cmd                    ← رابط گرافیکی
  │  ├─ say.cmd                    ← خط فرمان
  │  ├─ get_model.cmd              ← دانلود صدا
  │  └─ diagnose.cmd               ← رفع مشکل
  ├─ runtime\                      ← موتور
  ├─ models\                       ← صداها
  └─ out.wav                       ← فایل صوت خروجی
```

---

## 🚀 گزینه‌های پیشرفته

### استفاده از صدای دیگر
خط `MODEL_DIR` در `scripts\say.cmd` را ویرایش کنید:
```bat
set "MODEL_DIR=%CD%\models\vits-piper-fa_IR-amir-medium"
```

### اجرای سرور در پورت دلخواه
`scripts\server.cmd` را ویرایش کنید:
```bat
runtime\tts_server.exe --port 9000
```

### پردازش دسته‌ای
یک `.wav` برای هر سطر:
```bat
scripts\say_batch.cmd myfile.txt
```

---

## ❗ مشکلات رایج

| مشکل | راه حل |
|------|---------|
| "????" به جای فارسی | از `--file` استفاده کنید |
| بدون صدا | CPU قدیم؟ اجرای `scripts\diagnose.cmd` |
| خطای DLL | Visual C++ 2015-2022 را نصب کنید |
| خطای "procedure entry point" | ZIP را دوباره استخراج کنید |
| سرور راه‌اندازی نمی‌شود | پورت 8756 درحال استفاده است. پورت را تغییر دهید |

---

## 📊 کارایی

| مدل | سرعت | کیفیت | حجم |
|-----|------|-------|------|
| gyro (پیشفرض) | سریع | بهترین | 450 MB |
| ganji | خیلی سریع | خوب | 150 MB |
| haaniye | سریع | خوب | 100 MB |

---

## 📜 مجوز و اعتبار

- **sherpa-onnx** (k2-fsa) — Apache-2.0
- **ONNX Runtime** (Microsoft) — MIT
- **مدل‌های Piper / VITS** — هر کدام مجوز خود را دارد
- **این ابزار** — MIT

مدل‌های فارسی توسط [karim23657](https://huggingface.co/karim23657)

---

## 🤝 کمک نیاز دارید؟

1. **سوالات متداول را بخوانید** — اغلب سوالات پاسخ داده شده‌اند
2. **`scripts\diagnose.cmd` را اجرا کنید** — خروجی را کپی کنید
3. **به کانال تلگرام ما بپیوندید** — **[📱 کانال تلگرام](https://t.me/persian_tts)** 
4. **یک issue بازکنید** — [GitHub Issues](https://github.com/karim23657/offline-persian-tts-win7/issues)

---

## 🌍 کاربردها

✅ ایجاد کتاب‌های صوتی فارسی  
✅ تولید صدای فیلم‌ها  
✅ ابزار دسترسی‌پذیری  
✅ چت‌بات‌های صوتی  
✅ یادگیری تلفظ  
✅ استفاده در ماشین‌های قدیم  
✅ رابط وب برای استفاده مشترک در شبکه  

---

**🎉 آماده‌اید؟ [الان دانلود کنید](https://github.com/karim23657/offline-persian-tts-win7/releases/latest)**

استخراج → اجرای `scripts\server.cmd` → باز کردن مرورگر → تایپ فارسی → شنیدن صدا. انجام شد!
