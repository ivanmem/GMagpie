// Run the production photo-gallery logic on CPU only: no window, no device,
// no GMagpie process. Doubles for the pipeline types describe the ratios the real
// effects use (ACNet/Anime4K double the frame); the assertions are the defects
// the slideshow had, kept as regressions.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// Тест проверяет поведение через assert(); сборка с NDEBUG превратила бы его в «всё хорошо»
#ifdef NDEBUG
#error "Photo gallery tests rely on assert(): build them without NDEBUG"
#endif
// min/max из <Windows.h> ломают std::min/std::max в извлечённом коде
#define NOMINMAX
#include <Windows.h>

namespace fs = std::filesystem;

namespace GMagpie {

// --- stand-ins for the types the extracted code touches -------------------
// Only the members the production code reads exist here, so a change in what it
// reads stops compiling instead of silently changing behaviour.

struct EffectDesc {
	float scale = 1.f;
	bool unpredictable = false;
};

struct EffectOption {
	std::string name;
};

struct EffectDrawer {
	// Same contract as the real EffectDrawer::CalcOutputSize: predictable result or
	// a non-positive size when the stage cannot be predicted.
	static SIZE CalcOutputSize(const EffectDesc& desc, const EffectOption&, SIZE input) noexcept {
		if (desc.unpredictable) {
			return { 0, 0 };
		}
		return { static_cast<LONG>(std::lround(input.cx * desc.scale)),
			static_cast<LONG>(std::lround(input.cy * desc.scale)) };
	}
};

struct _TestLog {
	int errors = 0;
	template<class... Args> void Error(Args&&...) noexcept { ++errors; }
};
struct Logger {
	static _TestLog& Get() noexcept { static _TestLog log; return log; }
};

struct StrHelper {
	static std::string UTF16ToUTF8(const std::wstring&) { return {}; }
};

namespace fmt {
template<class... Args> std::string format(std::string_view, Args&&...) { return {}; }
}

#include "PhotoGalleryUnderTest.h"

}

using namespace GMagpie;

namespace {

constexpr SIZE SCREEN = { 2560, 1440 };

std::vector<EffectOption> Options(size_t count) {
	std::vector<EffectOption> effects;
	for (size_t i = 0; i < count; ++i) {
		effects.push_back(EffectOption{ "x2-" + std::to_string(i) });
	}
	return effects;
}

std::vector<EffectDesc> Scales(std::initializer_list<float> scales) {
	std::vector<EffectDesc> descs;
	for (float scale : scales) {
		descs.push_back(EffectDesc{ scale, false });
	}
	return descs;
}

void TestStageCount() {
	// Три удвоения, как профиль «Low Energy VSR» (ACNet + ACNet + Anime4K)
	auto triple = Options(3);
	auto x2 = Scales({ 2.f, 2.f, 2.f });

	// Фотография ровно размера экрана: апскейл не нужен ни на одном этапе. Это тот
	// случай, из-за которого кадр отбрасывался («показала предыдущую фотографию»)
	// и падал процесс: 0 этапов сам по себе корректен, недопустима пустая цепь.
	assert(CalcPhotoEffectStages(triple, x2, { 2560, 1440 }, SCREEN) == 0);
	// больше экрана — тоже
	assert(CalcPhotoEffectStages(triple, x2, { 4096, 2304 }, SCREEN) == 0);
	// и «чуть больше» (3072x1728 на экране 2560x1440): уменьшение — не этап цепи
	assert(CalcPhotoEffectStages(triple, x2, { 3072, 1728 }, SCREEN) == 0);

	// 1280x720 -> 2560x1440 ровно за один этап
	assert(CalcPhotoEffectStages(triple, x2, { 1280, 720 }, SCREEN) == 1);
	// портретная страница альбома: цель ограничена высотой, этапа всё равно один
	assert(CalcPhotoEffectStages(triple, x2, { 800, 1400 }, SCREEN) == 1);
	// 1200x674 -> вписать = 2560x1438, одного удвоения (2400x1348) не хватает
	assert(CalcPhotoEffectStages(triple, x2, { 1200, 674 }, SCREEN) == 2);
	// маленькой фотографии нужны все этапы, что есть в профиле
	assert(CalcPhotoEffectStages(triple, x2, { 320, 180 }, SCREEN) == 3);

	// «Предохранитель»: этап, выход которого забирается за предел текстуры, не должен
	// укорачивать цепь — остаются все этапы, лишнее уберёт нормализующий проход
	auto two = Options(2);
	assert(CalcPhotoEffectStages(two, Scales({ 10.f, 2.f }), { 1024, 768 }, SCREEN) == 2);
	// непредсказуемый этап — тот же ответ
	assert(CalcPhotoEffectStages(two, { EffectDesc{ 2.f, true }, EffectDesc{ 2.f, false } },
		{ 1024, 768 }, SCREEN) == 2);

	// Описания не совпадают со списком эффектов — предсказать нельзя, цепь не трогаем
	assert(CalcPhotoEffectStages(triple, Scales({ 2.f, 2.f }), { 1280, 720 }, SCREEN) == 3);
	// пустой плейлист эффектов (только нормализация) и нулевые размеры
	assert(CalcPhotoEffectStages({}, {}, { 1280, 720 }, SCREEN) == 0);
	assert(CalcPhotoEffectStages(triple, x2, { 0, 0 }, SCREEN) == 3);
	assert(CalcPhotoEffectStages(triple, x2, { 1280, 720 }, { 0, 0 }) == 3);

	// Другой монитор: та же фотография требует больше этапов на большем экране и
	// меньше на меньшем — предсказание привязано к кадру, а не к профилю
	assert(CalcPhotoEffectStages(triple, x2, { 1280, 720 }, { 3840, 2160 }) == 2);
	assert(CalcPhotoEffectStages(triple, x2, { 1280, 720 }, { 1920, 1080 }) == 1);
	// предельный размер: единственный этап, который выводит за 8192, не отбрасывается —
	// он нужен, чтобы дойти до кадра 8K, а лишнее уберёт нормализующий проход
	assert(CalcPhotoEffectStages(Options(1), Scales({ 3.f }), { 4096, 2304 }, { 7680, 4320 }) == 1);

	std::cout << "Photo stage count: no-upscale, portrait, short-of-one-stage, all-stages, "
		"texture fuse, unpredictable, other displays and invalid inputs passed.\n";
}

// Путь страницы в тестовом альбоме: по частям, чтобы разделители были системными —
// обход диска возвращает именно такие строки
fs::path PagePath(const fs::path& root, std::string_view name) {
	fs::path path = root;
	size_t pos = 0;
	while (true) {
		const size_t next = name.find('/', pos);
		path /= std::string(name.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
		if (next == std::string_view::npos) {
			break;
		}
		pos = next + 1;
	}
	return path;
}

void TestPlaylist() {
	// Альбом как он есть на диске: подпапки, разный регистр расширений, чужие файлы,
	// каталог с именем картинки и файл без расширения
	const fs::path root = fs::temp_directory_path()
		/ ("magpie-gallery-test-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code ec;
	fs::create_directories(root / "a", ec);
	assert(!ec);
	fs::create_directories(root / "b" / "deep", ec);
	// каталог с расширением картинки — не страница
	fs::create_directories(root / "cover.png", ec);
	assert(!ec);

	const char* files[] = {
		"001.bmp", "002.PNG", "notes.txt", "archive.zip", "noext", "double.gifs",
		"a/010.jpg", "a/010.tiff", "a/notes.md", "b/003.webp", "b/cover.JPEG",
		"b/deep/004.heic", "b/deep/005.jfif",
	};
	for (const char* name : files) {
		std::ofstream(PagePath(root, name)) << "x";
	}

	const std::vector<std::wstring> playlist = BuildPhotoPlaylist(root.native());

	// Порядок — «номера страниц», которые видит пользователь: сначала файлы корня,
	// затем подпапки по имени, внутри — по имени файла
	const std::vector<std::string> expected = {
		"001.bmp", "002.PNG", "a/010.jpg", "a/010.tiff",
		"b/003.webp", "b/cover.JPEG", "b/deep/004.heic", "b/deep/005.jfif",
	};
	assert(playlist.size() == expected.size());
	for (size_t i = 0; i < expected.size(); ++i) {
		assert(playlist[i] == PagePath(root, expected[i]).native());
	}

	// Чужие форматы, файл без расширения и каталог «cover.png» не стали страницами
	for (const std::wstring& page : playlist) {
		const std::wstring ext = fs::path(page).extension().wstring();
		std::wstring lower = ext;
		std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
		assert(SUPPORTED_EXTENSIONS.contains(lower));
		assert(fs::is_regular_file(page));
	}

	// Порядок — это «номера страниц», поэтому он упорядочен по полному пути и не
	// зависит от того, в каком порядке их вернул обход диска
	assert(std::ranges::is_sorted(playlist));
	assert(BuildPhotoPlaylist(root.native()) == playlist);
	// одно фото из Проводника: только соседи по папке, без подпапок
	assert((BuildPhotoPlaylist(root.native(), false) == std::vector<std::wstring>{
		PagePath(root, "001.bmp").native(), PagePath(root, "002.PNG").native() }));
	// внутри одной папки номер страницы совпадает с именем файла
	assert(std::ranges::find(playlist, fs::path(root / "a" / "010.jpg").native())
		< std::ranges::find(playlist, fs::path(root / "a" / "010.tiff").native()));

	// Несуществующая и пустая папки — пустой плейлист и сообщение в лог, не падение
	const int errorsBefore = Logger::Get().errors;
	assert(BuildPhotoPlaylist((root / "missing").native()).empty());
	assert(Logger::Get().errors == errorsBefore + 1);
	const fs::path empty = root / "empty";
	fs::create_directories(empty, ec);
	assert(BuildPhotoPlaylist(empty.native()).empty());

	fs::remove_all(root, ec);
	std::cout << "Playlist: recursion, mixed case, foreign files, directory-with-extension, "
		"deterministic order, missing and empty folders passed.\n";
}

void TestResumeIndex() {
	// Обычный случай: страница из прошлого запуска всё ещё в альбоме
	assert(PhotoResumeIndex(2, 158) == 2);
	// Альбом стал меньше (файлы удалили) — берём последную существующую, «Продолжить»
	// не должно открывать пустоту
	assert(PhotoResumeIndex(200, 158) == 157);
	assert(PhotoResumeIndex(157, 158) == 157);
	assert(PhotoResumeIndex(SIZE_MAX, 1) == 0);
	// Пустой альбом: Start отказывает раньше, но и здесь не должно быть деления на ноль
	assert(PhotoResumeIndex(0, 0) == 0);
	assert(PhotoResumeIndex(7, 0) == 0);

	std::cout << "Resume index: existing page, shrunk album, last page and empty album passed.\n";
}

void TestPrefetchDirection() {
	// автослайд и «вперёд»: следующий кадр по плейлисту, с переходом к началу
	assert(PhotoPrefetchIndex(0, 3, +1) == 1);
	assert(PhotoPrefetchIndex(1, 3, +1) == 2);
	assert(PhotoPrefetchIndex(2, 3, +1) == 0);
	// прокрутка назад раньше всегда шла по холодному следу: декод это p50 ~40 мс
	// из p90 ~63 мс всей задержки страницы
	assert(PhotoPrefetchIndex(2, 3, -1) == 1);
	assert(PhotoPrefetchIndex(0, 3, -1) == 2);
	// один альбом и пустой плейлист
	assert(PhotoPrefetchIndex(0, 1, -1) == 0);
	assert(PhotoPrefetchIndex(0, 0, +1) == 0);

	std::cout << "Prefetch direction: forward, backward, wrap-around, single and empty passed.\n";
}

void TestScalePercent() {
	// Строка состояния обещает масштаб, который действительно будет: 1200x674 на
	// 2560x1440 вписывается по ширине
	const double fit = PhotoScalePercent(1200, 674, 2560, 1440);
	assert(std::fabs(fit - 213.333) < 0.01);
	assert(std::lround(fit) == 213);
	// 100% — родной размер ровно вписывается, апскейл не нужен
	assert(std::fabs(PhotoScalePercent(2560, 1440, 2560, 1440) - 100.0) < 0.001);
	// портретная страница ограничена высотой
	assert(std::fabs(PhotoScalePercent(800, 1400, 2560, 1440) - 102.857) < 0.001);
	// фото больше экрана — уменьшение, проценты меньше сотни
	assert(std::fabs(PhotoScalePercent(3840, 2160, 2560, 1440) - 66.667) < 0.001);
	// размеры недоступны — 0, вызывающий покажет «?» вместо «0%» от деления на ноль
	assert(PhotoScalePercent(0, 674, 2560, 1440) == 0.0);
	assert(PhotoScalePercent(1200, 674, 0, 0) == 0.0);

	std::cout << "Scale percent: landscape, native, portrait, downscale and invalid sizes passed.\n";
}

void TestGalleryKeys() {
	using A = GalleryKeyAction;
	// Стрелки листают страницы, Num6 открывает контекстное меню, Esc и F11 выходят.
	// Пока меню открыто, клавиши принадлежат ему: хук вообще не зовёт эту карту
	assert(DecideGalleryKey(VK_LEFT) == A::PagePrev);
	assert(DecideGalleryKey(VK_UP) == A::PagePrev);
	assert(DecideGalleryKey(VK_RIGHT) == A::PageNext);
	assert(DecideGalleryKey(VK_DOWN) == A::PageNext);
	assert(DecideGalleryKey(VK_HOME) == A::First);
	assert(DecideGalleryKey(VK_END) == A::Last);
	assert(DecideGalleryKey(VK_SPACE) == A::TogglePause);
	assert(DecideGalleryKey(VK_NUMPAD6) == A::OpenMenu);
	assert(DecideGalleryKey(VK_DELETE) == A::DeletePhoto);
	assert(DecideGalleryKey(VK_ESCAPE) == A::Stop);
	// F11 останавливает показ, как Esc: от «полноэкранной» клавиши ждут выхода из просмотра;
	// во время показа чужой полноэкранный недоступен
	assert(DecideGalleryKey(VK_F11) == A::Stop);

	// Всё остальное должно доставаться обычным программам:
	// Enter в диалоге, цифры и буквы, правый Shift, F1
	for (unsigned vk : { (unsigned)VK_RETURN, (unsigned)VK_TAB, (unsigned)VK_BACK,
		(unsigned)VK_F1, (unsigned)VK_SHIFT, (unsigned)VK_CONTROL, (unsigned)VK_LWIN,
		(unsigned)VK_NUMPAD5, (unsigned)VK_ADD, (unsigned)'A', (unsigned)'6', 0xFFu }) {
		assert(DecideGalleryKey(vk) == A::PassThrough);
	}

	std::cout << "Gallery keys: paging, Num6 opens the context menu, Esc/F11 stop the show, "
		"Delete removes the file, everything else passes through.\n";
}

void TestPhotoDeleteIndex() {
	// Удалили файл из середины: на его место встаёт следующий — та же страница
	assert(PhotoIndexAfterDelete(2, 10) == std::optional<size_t>(2));
	assert(PhotoIndexAfterDelete(0, 2) == std::optional<size_t>(0));
	// последняя страница: следующий файл уже не существует — сдвиг на одну назад
	assert(PhotoIndexAfterDelete(9, 10) == std::optional<size_t>(8));
	assert(PhotoIndexAfterDelete(1, 2) == std::optional<size_t>(0));
	// альбом из одного файла: показывать нечего, останов
	assert(!PhotoIndexAfterDelete(0, 1));
	// вырожденные входы — тоже останов, без делений и вычитаний из нуля
	assert(!PhotoIndexAfterDelete(0, 0));

	std::cout << "Photo delete: next page takes the place, last page steps back, empty album "
		"stops the show.\n";
}

void TestGalleryLiteArgument() {
	// «Лёгкий» экземпляр — только запуск с --gallery и папкой после него: без
	// глобальных хоткеев и наблюдателя foreground он уживается с основным GMagpie
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"--gallery", (wchar_t*)L"D:\\album" };
		assert(HasGalleryArgument(3, argv));
	}
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"--interval", (wchar_t*)L"5",
			(wchar_t*)L"--gallery", (wchar_t*)L"D:\\album", (wchar_t*)L"--mode", (wchar_t*)L"X" };
		assert(HasGalleryArgument(7, argv));
	}
	// одна опция без папки — показ не начнётся, экземпляр остаётся обычным
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"--gallery" };
		assert(!HasGalleryArgument(2, argv));
	}
	// прочие запуски (в том числе «похожие» написания) — полный экземпляр как раньше
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"-t" };
		assert(!HasGalleryArgument(2, argv));
	}
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"--gallery=D:\\album" };
		assert(!HasGalleryArgument(2, argv));
	}
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe" };
		assert(!HasGalleryArgument(1, argv));
	}

	// Проводник открывает одно фото: «GMagpie.exe "<файл>"»; значения опций файлом не считаются
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"D:\\album\\Photo.JPG" };
		assert(FindPhotoArgument(2, argv) == L"D:\\album\\Photo.JPG");
	}
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"--mode", (wchar_t*)L"x.jpg", (wchar_t*)L"-t" };
		assert(FindPhotoArgument(4, argv).empty());
	}
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"D:\\doc\\readme.txt" };
		assert(FindPhotoArgument(2, argv).empty());
	}
	// путь к открываемому файлу: только существующий, нормализованный; нет файла - пусто
	{
		const fs::path photo = fs::temp_directory_path() / "gmagpie_open_arg" / "p.png";
		fs::create_directories(photo.parent_path());
		std::ofstream(photo) << "x";
		const std::wstring dotted = (photo.parent_path() / L"." / L"p.png").wstring();
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)dotted.c_str() };
		assert(OpenFileArgument(2, argv) == photo.lexically_normal().wstring());
		wchar_t* missing[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"D:\\no\\such\\file.png" };
		assert(OpenFileArgument(2, missing).empty());
		fs::remove_all(photo.parent_path());
	}
	{
		wchar_t* argv[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"--register-shell" };
		assert(FindShellCommand(2, argv) == std::optional<bool>(true));
		wchar_t* argv2[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"--unregister-shell" };
		assert(FindShellCommand(2, argv2) == std::optional<bool>(false));
		wchar_t* argv3[] = { (wchar_t*)L"GMagpie.exe", (wchar_t*)L"-t" };
		assert(!FindShellCommand(2, argv3).has_value());
	}

	std::cout << "Gallery lite mode: only --gallery with a folder makes the instance lite "
		"(own single-instance group, no global hotkeys), everything else stays a full one.\n";
}

void TestExifOrientationSwap() {
	// Повороты 5..8 (90/270°): на экран кадр уходит повёрнутым, проценты надо
	// считать по обменённым сторонам; остальные ориентации — как есть
	assert(!PhotoAppliesExifRotation(0));  // метаданных нет
	assert(!PhotoAppliesExifRotation(1));  // нормальная
	assert(!PhotoAppliesExifRotation(4));  // зеркало без поворота
	assert(PhotoAppliesExifRotation(5));
	assert(PhotoAppliesExifRotation(6));
	assert(PhotoAppliesExifRotation(7));
	assert(PhotoAppliesExifRotation(8));
	assert(!PhotoAppliesExifRotation(9));  // за границами списка — не выдумываем

	std::cout << "EXIF orientation: only 5..8 swap the displayed width and height, the "
		"status percent matches the rotated frame.\n";
}

void TestWheelPaging() {
	int accumulator = 0;

	// один полный щелчок — одна страница; знак «минус» — к предыдущей
	assert(WheelPageSteps(accumulator, 120) == 1);
	assert(accumulator == 0);
	assert(WheelPageSteps(accumulator, -120) == -1);
	assert(accumulator == 0);

	// два щелчка одним событием (свободное колесо)
	assert(WheelPageSteps(accumulator, 240) == 2);
	assert(accumulator == 0);

	// «плавное» колесо: доли не листают, но и не теряются
	assert(WheelPageSteps(accumulator, 40) == 0);
	assert(WheelPageSteps(accumulator, 40) == 0);
	assert(WheelPageSteps(accumulator, 40) == 1);
	assert(accumulator == 0);

	// встречные доли гасятся, страница не «уезжает» на прокрутке туда-сюда
	assert(WheelPageSteps(accumulator, 40) == 0);
	assert(WheelPageSteps(accumulator, -40) == 0);
	assert(accumulator == 0);

	// серия мелких событий назад: ровно одна страница на накопленный щелчок
	int pages = 0;
	for (int i = 0; i < 5; ++i) {
		pages += WheelPageSteps(accumulator, -30);
	}
	assert(pages == -1);
	assert(accumulator == -30);

	// накопитель всегда меньше щелчка — иначе следующий прогон листает лишнее
	for (int delta : { 119, 119, -119, 5000, -5000, 1 }) {
		WheelPageSteps(accumulator, static_cast<short>(delta));
		assert(accumulator > -WHEEL_DELTA && accumulator < WHEEL_DELTA);
	}

	// историческая регрессия: колесо вообще молчало (delta приходил нулём) —
	// ноль не должен листать и не должен копить
	assert(WheelPageSteps(accumulator, 0) == 0);

	std::cout << "Wheel paging: single notch, multi-notch, smooth fractions, counter-scroll "
		"and accumulator bounds passed.\n";
}

void TestStagePlacement() {
	// Два монитора 2560x1440 бок о бок: основной 0..2560, второй 2560..5120
	const RECT left{ 0, 0, 2560, 1440 };
	const RECT right{ 2560, 0, 5120, 1440 };
	const SIZE stage{ 512, 288 };

	// Курсор в середине второго — сцена целиком на втором, центром на курсоре
	RECT rc = PhotoStageRect({ 3840, 720 }, right, stage);
	assert(rc.left >= right.left && rc.right <= right.right);
	assert(rc.top >= right.top && rc.bottom <= right.bottom);
	assert(rc.left + stage.cx / 2 == 3840 && rc.top + stage.cy / 2 == 720);

	// Курсор у левого края второго — ни пикселя на основном: пересечение не с тем
	// монитором выбрало бы не тот экран
	rc = PhotoStageRect({ 2570, 100 }, right, stage);
	assert(rc.left >= right.left && rc.right <= right.right);
	assert(rc.top >= right.top && rc.bottom <= right.bottom);

	// Правый нижний угол — не выходит за границы монитора ни на пиксель
	rc = PhotoStageRect({ 5119, 1439 }, right, stage);
	assert(rc.left >= right.left && rc.right <= right.right);
	assert(rc.top >= right.top && rc.bottom <= right.bottom);

	// Верхний левый угол основного — тот же кламп в другую сторону
	rc = PhotoStageRect({ 1, 1 }, left, stage);
	assert(rc.left == 0 && rc.top == 0 && rc.right == 512 && rc.bottom == 288);

	// Монитор меньше сцены (дегенеративный): не уезжает за левый/верхний край
	const RECT tiny{ 1000, 1000, 1200, 1150 };
	rc = PhotoStageRect({ 1100, 1100 }, tiny, stage);
	assert(rc.left == 1000 && rc.top == 1000);

	std::cout << "Stage placement: the cursor's monitor wins, centered on the cursor, "
		"clamped at edges and tiny monitors passed.\n";
}

void TestPhotoSceneViewport() {
	const RECT frame{ 0, 0, 2560, 1440 };
	const POINT noOffset{ 0, 0 };
	const SIZE target{ 2560, 1440 };

	// Квадрат на 16:9-экране: масштабированная копия идёт в вертикальную полосу —
	// копия 1:1 игнорировала бы полосу и растягивала фото на весь экран
	RECT v = CalcPhotoSceneViewport({ 560, 0, 2000, 1440 }, frame, noOffset, target);
	assert(v.left == 560 && v.top == 0 && v.right == 2000 && v.bottom == 1440);

	// Портрет: своя узкая полоса
	v = CalcPhotoSceneViewport({ 787, 0, 1773, 1440 }, frame, noOffset, target);
	assert(v.left == 787 && v.right == 1773 && v.top == 0 && v.bottom == 1440);

	// Фото своим аспектом ровно в экран: пустой ответ — быстрая копия 1:1, без прорисовки
	v = CalcPhotoSceneViewport({ 0, 0, 2560, 1440 }, frame, noOffset, target);
	assert(v.left == 0 && v.top == 0 && v.right == 0 && v.bottom == 0);

	// Смещение кадра (независимый оверлейный слой): прямоугольник уезжает и обрезается целью
	v = CalcPhotoSceneViewport({ 560, 0, 2000, 1440 }, { 0, 40, 2560, 1480 }, noOffset, target);
	assert(v.left == 560 && v.top == 0 && v.right == 2000 && v.bottom == 1400);

	// Прямоугольник свешивается за правый край цели — отрезается по нему
	v = CalcPhotoSceneViewport({ 2460, 0, 2960, 1440 }, frame, noOffset, target);
	assert(v.left == 2460 && v.right == 2560);

	// Цель смещена drawOffset'ом — вьюпорт едет вместе с ней
	v = CalcPhotoSceneViewport({ 560, 0, 2000, 1440 }, frame, { 100, 0 }, target);
	assert(v.left == 660 && v.right == 2100);

	// Пустой целевой прямоугольник (кадр ещё не посчитан) — быстрый путь, не деление на ноль
	v = CalcPhotoSceneViewport({ 0, 0, 0, 0 }, frame, noOffset, target);
	assert(v.left == 0 && v.top == 0 && v.right == 0 && v.bottom == 0);

	std::cout << "Photo scene viewport: the letterbox band is kept, an exact aspect fit "
		"takes the 1:1 path, target clipping and offsets passed.\n";
}

} // namespace

int main() {
	TestStageCount();
	TestPlaylist();
	TestResumeIndex();
	TestPrefetchDirection();
	TestScalePercent();
	TestGalleryKeys();
	TestPhotoDeleteIndex();
	TestGalleryLiteArgument();
	TestExifOrientationSwap();
	TestWheelPaging();
	TestStagePlacement();
	TestPhotoSceneViewport();
}
