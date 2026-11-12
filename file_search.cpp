// ============================================================================
//  Поиск файлов по слову (C++17, Windows, консоль)
//
//  Программа обходит все локальные и съёмные диски (кроме C:\Windows) и
//  выводит пути файлов, подходящих под запрос. Два режима:
//    1 - слово ищется в ПУТИ к файлу (без учёта регистра, быстро);
//    2 - слово ищется ВНУТРИ файлов (без учёта регистра, в кодировках
//        UTF-8, ANSI, OEM и UTF-16LE), можно ограничить список расширений.
// ============================================================================

// Отключаем макросы min/max из windows.h: иначе они конфликтуют с std::max/std::min.
// Директиву нужно ставить ДО подключения <windows.h>.
#define NOMINMAX

#include <iostream>
#include <windows.h>     // WinAPI: консоль, диски, перекодировка строк
#include <filesystem>    // обход файловой системы (C++17)
#include <fstream>       // чтение файлов
#include <string>
#include <vector>
#include <set>
#include <sstream>       // разбор строки с расширениями
#include <algorithm>     // std::transform, std::max
#include <cwctype>       // std::towlower / std::towupper для wchar_t
#include <cstring>       // std::memcmp

namespace fs = std::filesystem;

// ============================================================================
//  КОНСОЛЬ
// ============================================================================

// Выводит юникодную строку в консоль заданным цветом, затем возвращает
// стандартный (белый) цвет. WriteConsoleW выводит UTF-16 напрямую, поэтому
// имена файлов с любыми символами печатаются корректно, без исключений
// из-за кодировки (в отличие от path.string() и std::cout).
void print(const std::wstring& s, WORD color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);   // хендл стандартного вывода
    SetConsoleTextAttribute(h, color);            // ставим нужный цвет текста
    DWORD written = 0;                            // сколько символов записано (нам не нужно)
    WriteConsoleW(h, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
    // Возвращаем белый цвет по умолчанию
    SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// Читает строку из консоли в юникоде (UTF-16). Так русские слова вводятся
// без искажений независимо от кодовой страницы консоли.
std::wstring readLine() {
    wchar_t buf[1024];
    DWORD n = 0;                                  // сколько символов реально прочитано
    ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), buf, 1024, &n, nullptr);
    std::wstring s(buf, n);
    // ReadConsoleW оставляет в конце "\r\n" - убираем их
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

// ============================================================================
//  УТИЛИТЫ
// ============================================================================

// Приводит строку к нижнему регистру. Для wchar_t нужен именно std::towlower
// (std::tolower работает с char и портит юникод). Работает и для кириллицы.
std::wstring toLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Кодирует ОДИН символ в нужную кодировку (cp - кодовая страница Windows).
// Результат кладёт в out. Возвращает false, если символ в этой кодировке
// представить нельзя (например, русская буква в cp1252).
bool encodeChar(wchar_t c, UINT cp, std::string& out) {
    // UTF-16LE: байты символа в памяти уже и есть нужное представление
    // (1200 - условный код UTF-16LE, в WideCharToMultiByte не используется)
    if (cp == 1200) {
        out.assign(reinterpret_cast<const char*>(&c), sizeof(wchar_t));
        return true;
    }

    char buf[8];                 // для одного символа хватает с запасом
    BOOL usedDefault = FALSE;    // станет TRUE, если символ заменён на "?"

    // Для UTF-8 запрещаем некорректные символы; для однобайтовых кодировок
    // запрещаем "ближайшие" замены (best fit), чтобы не было ложных совпадений
    DWORD flags = (cp == CP_UTF8) ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;

    // Для UTF-8 параметр usedDefault передавать нельзя (будет ошибка),
    // поэтому там передаём nullptr
    int n = WideCharToMultiByte(cp, flags, &c, 1, buf, sizeof(buf), nullptr,
                                (cp == CP_UTF8) ? nullptr : &usedDefault);

    // n <= 0 - ошибка; usedDefault - символ не нашёлся в кодировке
    if (n <= 0 || usedDefault) return false;
    out.assign(buf, n);
    return true;
}

// ============================================================================
//  ШАБЛОН ДЛЯ ПОИСКА БЕЗ УЧЁТА РЕГИСТРА В БАЙТАХ
//
//  Идея: файл читается как набор байтов, поэтому регистр нельзя убрать
//  приведением строки. Вместо этого для каждой буквы искомого слова храним
//  байтовые варианты (нижний и верхний регистр) и при сравнении допускаем
//  любой из них. Один шаблон соответствует одной кодировке.
// ============================================================================
struct Pattern {
    // Для каждого символа слова - список допустимых байтовых последовательностей
    // (например, для 'п' в cp1251: "\xEF" и "\xCF")
    std::vector<std::vector<std::string>> chars;

    // Таблица возможных ПЕРВЫХ байтов совпадения: позволяет быстро отбрасывать
    // позиции в данных, где совпадение начаться не может
    bool first[256] = {};

    // Максимальная длина совпадения в байтах (нужна для "хвоста" между кусками)
    size_t maxLen = 0;
};

// Строит шаблон для слова в кодировке cp. Возвращает false, если хотя бы один
// символ слова в этой кодировке представить нельзя (такую кодировку пропускаем).
bool buildPattern(const std::wstring& word, UINT cp, Pattern& p) {
    for (wchar_t c : word) {
        std::set<std::string> alts;   // set убирает дубли (у цифр оба регистра совпадают)
        std::string e;

        // Два варианта символа: нижний и верхний регистр
        wchar_t variants[2] = { static_cast<wchar_t>(std::towlower(c)),
                                static_cast<wchar_t>(std::towupper(c)) };

        // Кодируем каждый вариант; непредставимые отбрасываем
        for (wchar_t v : variants)
            if (encodeChar(v, cp, e)) alts.insert(e);

        if (alts.empty()) return false;   // символа нет в этой кодировке

        // Считаем максимально возможную длину этого символа в байтах
        size_t mx = 0;
        for (auto& a : alts) mx = std::max(mx, a.size());
        p.maxLen += mx;

        p.chars.emplace_back(alts.begin(), alts.end());
    }

    // Заполняем таблицу первых байтов по первому символу слова
    for (auto& a : p.chars[0])
        p.first[static_cast<unsigned char>(a[0])] = true;
    return true;
}

// Проверяет, совпадает ли шаблон, начиная с позиции pos в данных d (длиной len).
// idx - номер символа слова, который сравниваем сейчас.
// Рекурсия нужна, потому что у символа может быть несколько вариантов байтов,
// и надо перебрать их все (перебор с возвратом).
bool matchAt(const Pattern& p, const char* d, size_t len, size_t pos, size_t idx) {
    if (idx == p.chars.size()) return true;   // все символы слова совпали

    for (const auto& alt : p.chars[idx]) {
        // Хватает ли байтов, совпадает ли вариант и совпадает ли остаток слова
        if (pos + alt.size() <= len &&
            std::memcmp(d + pos, alt.data(), alt.size()) == 0 &&
            matchAt(p, d, len, pos + alt.size(), idx + 1))
            return true;
    }
    return false;
}

// Ищет шаблон в блоке данных. Начинает проверку только с тех позиций, где
// байт может быть первым байтом совпадения (быстрый отсев по таблице first).
bool findPattern(const Pattern& p, const std::string& data) {
    const char* d = data.data();
    size_t len = data.size();
    for (size_t i = 0; i < len; ++i) {
        if (!p.first[static_cast<unsigned char>(d[i])]) continue;   // быстрый отсев
        if (matchAt(p, d, len, i, 0)) return true;
    }
    return false;
}

// ============================================================================
//  ПОИСК В СОДЕРЖИМОМ ФАЙЛА
// ============================================================================

// Возвращает true, если в файле найден любой из шаблонов (любая кодировка).
// maxLen - длина самого длинного шаблона (для сохранения "хвоста").
bool fileContainsAny(const fs::path& filePath, const std::vector<Pattern>& patterns, size_t maxLen) {
    // Бинарное чтение: никаких преобразований переводов строк и кодировок
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return false;                 // не удалось открыть (занят, нет доступа)

    const size_t CHUNK = 1 << 20;            // читаем по 1 МБ, чтобы не грузить файл целиком
    std::vector<char> tmp(CHUNK);
    std::string carry;                       // "хвост" предыдущего куска

    while (file) {
        file.read(tmp.data(), CHUNK);
        std::streamsize n = file.gcount();   // сколько байт реально прочитано
        if (n <= 0) break;

        // К новому куску приклеиваем хвост предыдущего: так находится слово,
        // которое попало на границу двух кусков
        std::string data = carry;
        data.append(tmp.data(), static_cast<size_t>(n));

        for (const auto& p : patterns)
            if (findPattern(p, data)) return true;

        // В хвост сохраняем последние (maxLen - 1) байт: слово длиной maxLen
        // не поместилось бы в них целиком, значит повторно мы его не найдём,
        // а начало слова на границе - сохраним
        size_t keep = maxLen > 0 ? maxLen - 1 : 0;
        carry = data.size() > keep ? data.substr(data.size() - keep) : data;
    }
    return false;
}

// ============================================================================
//  НАСТРОЙКИ ПОИСКА
// ============================================================================
struct Config {
    bool inContent = false;               // false = искать в пути, true = внутри файлов
    std::wstring wordLower;               // слово в нижнем регистре (режим "в пути")
    std::vector<Pattern> patterns;        // шаблоны по кодировкам (режим "в файле")
    size_t maxLen = 1;                    // длина самого длинного шаблона
    std::set<std::wstring> extensions;    // нужные расширения (режим "в файле"); пусто = все
};

// ============================================================================
//  ОБХОД ФАЙЛОВОЙ СИСТЕМЫ
// ============================================================================

// Рекурсивно обходит rootPath и печатает файлы, подходящие под настройки cfg.
// Вместо try/catch вокруг каждого вызова используется std::error_code, а общий
// try/catch в теле цикла защищает программу от любых неожиданных исключений.
void searchInFiles(const fs::path& rootPath, const Config& cfg) {
    std::error_code ec;

    // skip_permission_denied - папки без доступа пропускаются молча,
    // а не вызывают исключение
    fs::recursive_directory_iterator it(rootPath, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;    // "конец" обхода
    if (ec) return;                          // корень диска недоступен

    while (it != end) {
        try {
            const fs::path& path = it->path();

            // native() возвращает std::wstring без перекодировки. Нельзя
            // использовать string(): на Windows он бросает исключение для
            // символов, которых нет в текущей ANSI-кодовой странице
            const std::wstring& native = path.native();

            // Пропускаем C:\Windows целиком: проверяем НАЧАЛО пути и запрещаем
            // заходить внутрь этой папки
            if (native.rfind(L"C:\\Windows", 0) == 0) {
                it.disable_recursion_pending();
            }
            // Интересуют только обычные файлы (не папки, не ссылки и т.п.)
            else if (it->is_regular_file(ec) && !ec) {
                bool found = false;

                if (!cfg.inContent) {
                    // Режим 1: слово в пути, без учёта регистра
                    found = toLower(native).find(cfg.wordLower) != std::wstring::npos;
                }
                else {
                    // Режим 2: слово в содержимом.
                    // Сначала фильтр по расширению (пустой список = любые файлы)
                    bool extOk = cfg.extensions.empty() ||
                                 cfg.extensions.count(toLower(path.extension().native()));
                    // Читаем файл только если расширение подошло
                    found = extOk && fileContainsAny(path, cfg.patterns, cfg.maxLen);
                }

                // Совпадение: путь в зелёном (по пути) или красном (по содержимому) цвете
                if (found)
                    print(native + L"\n", cfg.inContent ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                                                        : FOREGROUND_GREEN);
            }
        }
        catch (...) {
            // Любой проблемный элемент просто пропускаем
        }

        // Переход к следующему элементу. При ошибке (например, диск отключили)
        // прекращаем обход этого диска
        ec.clear();
        it.increment(ec);
        if (ec) break;
    }
}

// Перебирает все логические диски и запускает поиск на локальных и съёмных.
void listDrivesAndSearch(const Config& cfg) {
    // Битовая маска: бит 0 = диск A, бит 1 = диск B, и т.д.
    DWORD drives = GetLogicalDrives();
    if (drives == 0) return;                 // не удалось получить список дисков

    for (wchar_t d = L'A'; d <= L'Z'; ++d) {
        if (drives & (1 << (d - L'A'))) {    // диск с такой буквой существует
            std::wstring name = std::wstring(1, d) + L":\\";   // например, L"C:\\"

            // Нужны только жёсткие (FIXED) и съёмные (REMOVABLE) диски;
            // сетевые, CD-ROM и RAM-диски пропускаем
            UINT type = GetDriveTypeW(name.c_str());
            if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
                print(L"\nСканируем диск: " + name + L"\n");
                searchInFiles(name, cfg);
            }
        }
    }
}

// ============================================================================
//  ТОЧКА ВХОДА
// ============================================================================
int main() {
    setlocale(LC_ALL, "ru");
    Config cfg;

    // 1. Выбор режима поиска
    print(L"Где искать слово?\n  1 - в пути к файлу\n  2 - внутри файлов\nВыбор: ");
    std::wstring choice = readLine();
    cfg.inContent = (choice == L"2");        // всё, кроме "2", считается режимом "в пути"

    // 2. Ввод слова
    print(L"Введите слово для поиска: ");
    std::wstring word = readLine();
    if (word.empty()) {
        print(L"Слово не может быть пустым.\n");
        return 1;
    }

    if (!cfg.inContent) {
        // Режим "в пути": достаточно запомнить слово в нижнем регистре
        cfg.wordLower = toLower(word);
    }
    else {
        // Режим "в файле": строим шаблон для каждой кодировки, в которой
        // слово можно записать: UTF-8, ANSI, OEM (cp866), UTF-16LE (1200)
        const UINT encodings[] = { CP_UTF8, CP_ACP, CP_OEMCP, 1200 };
        for (UINT cp : encodings) {
            Pattern p;
            if (buildPattern(word, cp, p)) {
                cfg.maxLen = std::max(cfg.maxLen, p.maxLen);   // нужен для "хвоста" между кусками
                cfg.patterns.push_back(std::move(p));
            }
        }

        // 3. Ввод расширений (только для режима "в файле")
        print(L"Расширения через пробел (например: .txt .cpp .log), пусто = все файлы: ");
        std::wistringstream iss(readLine());
        for (std::wstring e; iss >> e; ) {   // разбиваем строку по пробелам
            if (e[0] != L'.') e = L"." + e;  // "txt" превращаем в ".txt"
            cfg.extensions.insert(toLower(e));
        }
    }

    // 4. Запуск поиска по всем дискам
    listDrivesAndSearch(cfg);
    return 0;
}
