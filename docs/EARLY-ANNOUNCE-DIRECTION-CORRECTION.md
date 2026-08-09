# پچِ ماژول — اعلامِ زودهنگامِ مالک + اصلاحِ جهت بدونِ ثبتِ ترددِ تکراری

هدف: به‌محضِ خواندنِ مطمئنِ پلاک، **مالک فوراً اعلام** شود (حتی اگر جهت هنوز نامعلوم/اشتباه باشد)،
و وقتی جهت پس از چند فریم قطعی شد، یک **پیامِ اصلاحیه** فرستاده شود تا بک‌اند **همان ردیفِ تردد را
به‌روزرسانی کند، نه یک تردد جدید**.

> سمتِ بک‌اند کامل پیاده و مستقر شده است (commit `0058742`). این سند فقط تغییراتِ لازمِ ماژولِ C++ است.

---

## ۱. قراردادِ بک‌اند (چه چیزی باید بفرستید)

ماژول باید برای **هر عبور** یک **شناسهٔ پایدارِ عبور (pass id)** بسازد و آن را در `plate.track_id`
هم روی **پیامِ زودهنگام** و هم روی **پیامِ اصلاحیه** با **مقدارِ یکسان** بفرستد.

بک‌اند (`TrafficService::record_detection`) در بازهٔ **۱۲۰ ثانیه** روی همان دوربین:
- اگر ردیفی با همان `track_id` بود → همان را **UPDATE** می‌کند (فقط جهت؛ و اگر خوانشِ جدید دقیق‌تر بود،
  تصویرِ پلاک/دقتِ OCR را هم ارتقا می‌دهد).
- **اگر `track_id` نباشد → هیچ ادغامی انجام نمی‌شود و همیشه ترددِ جدید ثبت می‌گردد** (رفتارِ قبلی، بدونِ
  ریسک). عمدی است: بدونِ شناسهٔ عبور نمی‌توان «اصلاحِ یک عبور» را از «دو عبورِ واقعیِ همان پلاک» تشخیص
  داد. **پس قابلیتِ اصلاح فقط وقتی فعال می‌شود که ماژول `track_id` بفرستد**؛ در حالتِ سادهٔ تشخیصِ پلاک
  (بدونِ شناسهٔ عبور) رفتار دقیقاً مثلِ قبل است و هیچ مشکلی ایجاد نمی‌شود.

مقادیرِ `direction`: `0=نامعلوم، 1=ورود، 2=خروج`.

### قطبیتِ Entry/Exit — دو حالت
از این پس تعیینِ «نزدیک‌شدن به دوربین = ورود یا خروج؟» می‌تواند در **بک‌اند** (per-camera) باشد:
- **حالتِ فعلی (پیش‌فرض):** ماژول خودش قطبیت را اعمال می‌کند و `direction` را به‌صورتِ **منطقی**
  (۱=ورود، ۲=خروج) می‌فرستد. در بک‌اند `cameras.approaching_is_entry = NULL` بماند → بک‌اند دست نمی‌زند.
- **حالتِ جدید (توصیه‌شده):** ماژول فقط trendِ **فیزیکی** را بفرستد — **۱ = نزدیک‌شدن (بزرگ‌شدنِ پلاک)،
  ۲ = دورشدن، ۰ = نامعلوم** — و قطبیت را اعمال **نکند**. سپس در تنظیماتِ همان دوربین در بک‌اند،
  `approaching_is_entry` را روی `true` (نزدیک‌شدن=ورود) یا `false` (نزدیک‌شدن=خروج) بگذارید؛ بک‌اند نگاشت
  را انجام می‌دهد. مزیت: تغییرِ قطبیت بدونِ build/دیپلویِ ماژول، از UI.

> برای هر دوربین یکی از دو حالت را انتخاب کنید و **سازگار** نگه دارید: اگر `approaching_is_entry` در بک‌اند
> set شد، ماژولِ همان دوربین باید trendِ فیزیکی بفرستد (نه منطقی)، وگرنه نگاشت دوباره اعمال و جهت وارونه
> می‌شود. `direction=0` در هر دو حالت «نامشخص» می‌ماند.

> چون در مدل، خودرو **ردیابی نمی‌شود**، `track_id` را از **وضعیتِ per-pass خودِ ماژول** بسازید
> (یک UUID برای هر عبور) — نیازی به مدلِ tracking نیست. همین که early و correction یک `track_id`
> مشترک داشته باشند کافی است.

---

## ۲. تغییرات (فایل‌به‌فایل)

### ۲-۱. یک `PassId` پایدار به‌ازای هر عبور
در `DetectionWorker` یک نگاشتِ کوچک نگه دارید: کلید `gate + ":" + text` → `{ passId, lastSeenMs, emittedEarly, lastSentDir }`.
- اگر ورودی نبود یا فاصلهٔ زمانی از آخرین دیده‌شدن > `passGapMs` (مثلاً ۵۰۰۰ms) بود → **عبورِ جدید**:
  یک `passId` تازه (UUID) بساز، `emittedEarly=false`, `lastSentDir=-1`.
- در غیر این صورت همان `passId` را ادامه بده.

هدر (`include/lpr/manager/DetectionWorker.h`):
```cpp
struct PassState { std::string passId; long lastSeenMs=0; bool emittedEarly=false; int lastSentDir=-1; };
std::unordered_map<std::string, PassState> passes_;   // key: gate + ":" + text
std::string passIdFor(const std::string& gate, const std::string& text, long nowMs); // impl below
```

پیاده‌سازی (`src/manager/DetectionWorker.cpp`) — از همان `generateUuidV4()` که `PlateSender` دارد استفاده کنید:
```cpp
std::string DetectionWorker::passIdFor(const std::string& gate, const std::string& text, long nowMs) {
    const std::string k = gate + ":" + text;
    auto& st = passes_[k];
    if (st.passId.empty() || (nowMs - st.lastSeenMs) > cfg_.passGapMs /*=5000*/) {
        st = PassState{ generateUuidV4(), nowMs, false, -1 };
    }
    st.lastSeenMs = nowMs;
    return st.passId;
}
```

### ۲-۲. اعلامِ زودهنگام (به‌جای نگه‌داشتن)
در `process()` (حدود خطِ ۳۱۰) بلوکِ «hold» را طوری تغییر دهید که به‌جای صرفاً نگه‌داشتن، **همان ابتدا
یک‌بار early** بفرستد و `track_id` را بگذارد:

```cpp
const std::string passId = passIdFor(item.gate, out.plate.text, nowMs);
out.plate.trackId = passId;                     // روی هر دو پیام یکسان می‌ماند
auto& st = passes_[item.gate + ":" + out.plate.text];

int dir = directionFor(item.gate, out.plate.text);   // 0 اگر هنوز قطعی نشده
if (out.plate.direction == 0) out.plate.direction = dir;   // بهترین حدسِ فعلی (شاید 0)

if (!st.emittedEarly) {
    // اعلامِ زودهنگام: مالک فوراً مشخص شود؛ جهت ممکن است 0 باشد.
    st.emittedEarly = true;
    st.lastSentDir  = out.plate.direction;
    emitPlate(std::move(out));                   // early
} else if (dir != 0 && dir != st.lastSentDir) {
    // جهت تغییر/قطعی شد → اصلاحیه (همان passId)، با دور زدنِ کول‌داون.
    out.plate.direction = dir;
    out.forceSend = true;                        // (بخش ۲-۴)
    st.lastSentDir = dir;
    emitPlate(std::move(out));                   // correction
}
// اگر early فرستاده شده و جهت هنوز عوض نشده → چیزی نفرست (از سیل جلوگیری می‌شود).
```

> بلوکِ `pending_`/`releasePending` قبلی برای «نگه‌داشتن تا قطعی‌شدنِ جهت» **دیگر لازم نیست** و باید
> حذف/غیرفعال شود؛ اصلاحیه جای آن را می‌گیرد. اگر می‌خواهید مرحله‌به‌مرحله بروید، می‌توانید
> `directionHoldMs=0` بگذارید تا early فوری برود، و منطقِ اصلاحیهٔ بالا را در `releasePending` هم
> (هنگام قطعی‌شدنِ جهت) صدا بزنید.

### ۲-۲-ب. پیامِ نهایی در پایانِ عبور (مورد: «یک‌بار دیده شد و دیگر نه»)
اگر خودرو فقط چند فریم دیده شود و برود، ممکن است جهت هرگز قطعی نشود. برای این حالت، وقتی عبور تمام
شد (پلاک برای بیش از `passGapMs` دیگر دیده نشد)، **یک پیامِ نهایی** با بهترین جهتِ ممکن و همان `passId`
بفرستید:
```cpp
// در حلقهٔ پاک‌سازیِ passes_ (یا یک تیکِ زمان‌بندی‌شده): عبورهایی که passGapMs از آخرین دیده‌شدنشان گذشته
for (auto it = passes_.begin(); it != passes_.end();) {
    auto& st = it->second;
    if (nowMs - st.lastSeenMs > cfg_.passGapMs) {
        int finalDir = /* directionForKey(it->first) */ 0;   // بهترین جهت؛ اگر قطعی نشد → 0
        if (st.emittedEarly && finalDir != st.lastSentDir) {
            // پیامِ نهایی با passId؛ direction ممکن است 0 بماند (نامشخص).
            emitCorrection(it->first, st.passId, finalDir);   // forceSend=true
        }
        it = passes_.erase(it);   // پاک‌سازی
    } else ++it;
}
```
- **دوربینِ دوطرفه:** اگر جهت واقعاً تعیین نشد، **`direction=0` (نامشخص)** بفرستید — بک‌اند آن را به‌صورتِ
  «نامشخص» ذخیره و نمایش می‌دهد (نه «خروجِ» اشتباه). ستونِ `traffic.direction` و UI این حالت را دارند.
- اگر لِینِ یک‌طرفه داشتید، می‌توانستید جهتِ پیش‌فرضِ دوربین را بگذارید؛ ولی چون دوربین‌ها دوطرفه‌اند،
  نامشخص ماندن درست‌تر است.

### ۲-۳. `forceSend` روی `PlateItem`
در تعریفِ `PlateItem` (کنارِ `plate/gate/timestamp/image`):
```cpp
bool forceSend = false;   // اصلاحیه: از کول‌داونِ gate:plate عبور کند
```

### ۲-۴. عبورِ اصلاحیه از کول‌داون در `PlateSender`
در `PlateSender::send(...)` جایی که `cooldownPasses(...)` را چک می‌کنید:
```cpp
if (!item.forceSend && !cooldownPasses(item.gate, item.plate.text, now)) return;  // early مثلِ قبل کول‌داون دارد؛ correction عبور می‌کند
```
`buildMessage` نیازی به تغییر ندارد — از قبل `plate.track_id = p.trackId` را می‌فرستد (خطِ ۷۸).

### ۲-۵. پیکربندی
- `passGapMs` (پیش‌فرض ۵۰۰۰): فاصله‌ای که پس از آن، همان پلاک «عبورِ جدید» حساب می‌شود.
- `directionHoldMs`: دیگر برای «نگه‌داشتنِ early» استفاده نمی‌شود (early فوری می‌رود). می‌تواند به‌عنوانِ
  «حداکثر زمانِ انتظار برای اصلاحیه» بماند یا حذف شود.
- (اختیاری) `earlyAnnounceEnable` برای روشن/خاموش کردنِ کلِ رفتار.

---

## ۳. نکات و لبه‌ها
- **ثباتِ متنِ پلاک:** چون fallbackِ بک‌اند به «پلاکِ دقیق» تکیه دارد، بهتر است early و correction **یک متنِ
  پلاکِ یکسان** (بهترین OCRِ عبور) بفرستند. با فرستادنِ `track_id`ِ مشترک، حتی اگر متن کمی فرق کند،
  هم‌بستگی درست انجام می‌شود (track_id اولویت دارد).
- **یکتاییِ `track_id`:** UUID است، پس سراسری یکتاست؛ نگرانیِ بازیافت (مثلِ idهای ByteTrack) وجود ندارد.
- **جلوگیری از سیل:** فقط دو پیام به‌ازای هر عبور (early + یک correction هنگام قطعی‌شدنِ جهت). اگر جهت
  چند بار عوض شد، هر تغییر یک اصلاحیه می‌فرستد (بک‌اند idempotent است و همان ردیف را آپدیت می‌کند).
- **پاک‌سازیِ `passes_`:** ورودی‌های قدیمی‌تر از چند دقیقه را دوره‌ای پاک کنید (مثلِ منطقِ prune در
  `cooldownPasses`) تا نگاشت رشد نکند.

---

## ۴. تستِ سرتاسری (پیشنهادی)
1. خودرویی که وارد می‌شود: باید ابتدا یک پیامِ early (جهت شاید 0/اشتباه) و سپس یک correction (جهتِ درست)
   با **همان `track_id`** برود.
2. در بک‌اند: باید **یک ردیفِ تردد** ساخته شود و `is_entered` آن پس از correction اصلاح شود (نه دو ردیف).
   بررسی: `SELECT id, plate_number, is_entered, track_id, "timestamp" FROM traffic WHERE track_id = '<uuid>';`
   باید دقیقاً یک ردیف بدهد.
3. در UIِ زنده: مالک باید در لحظهٔ early دیده شود و جهت پس از correction اصلاح شود.
