#include <ncurses.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

// Subset of Kconfig: mainmenu, menu/endmenu, choice/endchoice, config with
// bool/int/string, prompt, range, help, and "default VALUE [if EXPR]" where
// EXPR uses symbols, !, &&, ||, =, != and parentheses. The extra "values"
// keyword restricts an int or string to a fixed list shown as a selector.
enum class Kind { Bool, Int, String, Choice };

struct Default {
    std::string value;
    std::string condition;
};

struct ChoiceOption {
    std::string symbol;
    std::string label;
};

struct Item {
    Kind kind = Kind::Bool;
    std::string symbol;
    std::string label;
    std::string help;
    std::string menu;
    std::vector<std::string> values;
    std::vector<ChoiceOption> options;
    std::vector<Default> defaults;
    bool hasRange = false;
    long rangeMin = 0;
    long rangeMax = 0;
    std::string value;
};

struct Model {
    std::string title = "audiox configuration";
    std::vector<Item> items;
};

constexpr int minimumRows = 22;
constexpr int minimumColumns = 72;
constexpr const char* forbiddenStringChars = "\"'\\#$`";

std::string trim(const std::string& value) {
    const std::string whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

int indentWidth(const std::string& line) {
    int width = 0;
    for (char c : line) {
        if (c == ' ') {
            ++width;
        } else if (c == '\t') {
            width = (width / 8 + 1) * 8;
        } else {
            break;
        }
    }
    return width;
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t index = 0;
    while (index < line.size()) {
        if (std::isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
            continue;
        }
        if (line[index] == '#') {
            break;
        }
        std::string token;
        if (line[index] == '"') {
            ++index;
            while (index < line.size() && line[index] != '"') {
                token.push_back(line[index++]);
            }
            ++index;
        } else {
            while (index < line.size() && !std::isspace(static_cast<unsigned char>(line[index]))) {
                token.push_back(line[index++]);
            }
        }
        tokens.push_back(token);
    }
    return tokens;
}

std::string joinFrom(const std::vector<std::string>& tokens, std::size_t start) {
    std::string result;
    for (std::size_t index = start; index < tokens.size(); ++index) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += tokens[index];
    }
    return result;
}

bool loadModel(const std::string& kconfigPath, Model& model, std::string& error) {
    std::ifstream input(kconfigPath);
    if (!input) {
        error = "cannot read " + kconfigPath;
        return false;
    }

    std::vector<std::string> menus;
    Item* current = nullptr;
    int choiceIndex = -1;
    ChoiceOption* currentOption = nullptr;
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(line);
    }

    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const std::string& raw = lines[lineIndex];
        const auto tokens = tokenize(raw);
        if (tokens.empty()) {
            continue;
        }
        const std::string& keyword = tokens[0];
        const std::string where = kconfigPath + ":" + std::to_string(lineIndex + 1) + ": ";

        if (keyword == "help" || keyword == "---help---") {
            const int helpIndent = indentWidth(raw);
            std::string text;
            while (lineIndex + 1 < lines.size()) {
                const std::string& next = lines[lineIndex + 1];
                if (!trim(next).empty() && indentWidth(next) <= helpIndent) {
                    break;
                }
                ++lineIndex;
                const std::string piece = trim(next);
                if (!piece.empty()) {
                    if (!text.empty()) {
                        text.push_back(' ');
                    }
                    text += piece;
                }
            }
            if (current) {
                current->help = text;
            } else if (choiceIndex >= 0) {
                model.items[static_cast<std::size_t>(choiceIndex)].help = text;
            }
            continue;
        }

        if (keyword == "mainmenu" && tokens.size() > 1) {
            model.title = tokens[1];
        } else if (keyword == "menu" && tokens.size() > 1) {
            menus.push_back(tokens[1]);
            current = nullptr;
        } else if (keyword == "endmenu") {
            if (menus.empty()) {
                error = where + "endmenu without menu";
                return false;
            }
            menus.pop_back();
            current = nullptr;
        } else if (keyword == "choice") {
            Item choice;
            choice.kind = Kind::Choice;
            choice.menu = menus.empty() ? std::string() : menus.back();
            model.items.push_back(choice);
            choiceIndex = static_cast<int>(model.items.size()) - 1;
            current = nullptr;
            currentOption = nullptr;
        } else if (keyword == "endchoice") {
            if (choiceIndex < 0 || model.items[static_cast<std::size_t>(choiceIndex)].options.empty()) {
                error = where + "empty or unmatched choice";
                return false;
            }
            Item& choice = model.items[static_cast<std::size_t>(choiceIndex)];
            choice.symbol = choice.options.front().symbol + "_CHOICE";
            choiceIndex = -1;
            currentOption = nullptr;
            current = nullptr;
        } else if (keyword == "config" && tokens.size() > 1) {
            if (choiceIndex >= 0) {
                model.items[static_cast<std::size_t>(choiceIndex)].options.push_back({tokens[1], tokens[1]});
                currentOption = &model.items[static_cast<std::size_t>(choiceIndex)].options.back();
                current = nullptr;
            } else {
                Item item;
                item.symbol = tokens[1];
                item.label = tokens[1];
                item.menu = menus.empty() ? std::string() : menus.back();
                model.items.push_back(item);
                current = &model.items.back();
            }
        } else if (keyword == "bool" || keyword == "int" || keyword == "string") {
            if (currentOption) {
                if (keyword != "bool") {
                    error = where + "choice options must be bool";
                    return false;
                }
                if (tokens.size() > 1) {
                    currentOption->label = tokens[1];
                }
            } else if (current) {
                current->kind = keyword == "bool" ? Kind::Bool : keyword == "int" ? Kind::Int : Kind::String;
                if (tokens.size() > 1) {
                    current->label = tokens[1];
                }
            }
        } else if (keyword == "prompt" && tokens.size() > 1) {
            if (current) {
                current->label = tokens[1];
            } else if (choiceIndex >= 0) {
                model.items[static_cast<std::size_t>(choiceIndex)].label = tokens[1];
            }
        } else if (keyword == "default" && tokens.size() > 1) {
            Default entry{tokens[1], {}};
            if (tokens.size() > 3 && tokens[2] == "if") {
                entry.condition = joinFrom(tokens, 3);
            }
            if (current) {
                current->defaults.push_back(entry);
            } else if (choiceIndex >= 0 && !currentOption) {
                model.items[static_cast<std::size_t>(choiceIndex)].defaults.push_back(entry);
            }
        } else if (keyword == "range" && tokens.size() > 2 && current) {
            current->hasRange = true;
            current->rangeMin = std::strtol(tokens[1].c_str(), nullptr, 10);
            current->rangeMax = std::strtol(tokens[2].c_str(), nullptr, 10);
        } else if (keyword == "values" && current) {
            current->values.assign(tokens.begin() + 1, tokens.end());
        }
    }

    if (choiceIndex >= 0 || !menus.empty()) {
        error = kconfigPath + ": unterminated choice or menu";
        return false;
    }
    if (model.items.empty()) {
        error = kconfigPath + ": no config entries";
        return false;
    }
    return true;
}

std::string symbolValue(const std::vector<Item>& items, const std::string& symbol) {
    for (const auto& item : items) {
        if (item.kind == Kind::Choice) {
            for (const auto& option : item.options) {
                if (option.symbol == symbol) {
                    return item.value == symbol ? "y" : "n";
                }
            }
        } else if (item.symbol == symbol) {
            return item.value;
        }
    }
    return symbol;
}

class ExpressionParser {
public:
    ExpressionParser(const std::vector<Item>& items, const std::string& text) : items_(items) {
        std::size_t index = 0;
        while (index < text.size()) {
            const char c = text[index];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++index;
            } else if ((c == '&' || c == '|') && index + 1 < text.size() && text[index + 1] == c) {
                tokens_.push_back(std::string(2, c));
                index += 2;
            } else if (c == '!' && index + 1 < text.size() && text[index + 1] == '=') {
                tokens_.push_back("!=");
                index += 2;
            } else if (c == '!' || c == '(' || c == ')' || c == '=') {
                tokens_.push_back(std::string(1, c));
                ++index;
            } else {
                std::string word;
                if (c == '"') {
                    ++index;
                    while (index < text.size() && text[index] != '"') {
                        word.push_back(text[index++]);
                    }
                    ++index;
                } else {
                    while (index < text.size() && !std::isspace(static_cast<unsigned char>(text[index])) &&
                           std::strchr("!()=&|", text[index]) == nullptr) {
                        word.push_back(text[index++]);
                    }
                }
                tokens_.push_back(word);
            }
        }
    }

    bool evaluate() { return parseOr(); }

private:
    const std::vector<Item>& items_;
    std::vector<std::string> tokens_;
    std::size_t position_ = 0;

    bool accept(const char* token) {
        if (position_ < tokens_.size() && tokens_[position_] == token) {
            ++position_;
            return true;
        }
        return false;
    }

    bool parseOr() {
        bool result = parseAnd();
        while (accept("||")) {
            result = parseAnd() || result;
        }
        return result;
    }

    bool parseAnd() {
        bool result = parseUnary();
        while (accept("&&")) {
            result = parseUnary() && result;
        }
        return result;
    }

    bool parseUnary() {
        if (accept("!")) {
            return !parseUnary();
        }
        if (accept("(")) {
            const bool result = parseOr();
            accept(")");
            return result;
        }
        if (position_ >= tokens_.size()) {
            return false;
        }
        const std::string left = symbolValue(items_, tokens_[position_++]);
        if (accept("=")) {
            return position_ < tokens_.size() && left == symbolValue(items_, tokens_[position_++]);
        }
        if (accept("!=")) {
            return position_ < tokens_.size() && left != symbolValue(items_, tokens_[position_++]);
        }
        return left == "y";
    }
};

bool isValidValue(const Item& item, const std::string& value) {
    switch (item.kind) {
        case Kind::Bool:
            return value == "y" || value == "n";
        case Kind::Choice:
            return std::any_of(item.options.begin(), item.options.end(),
                               [&](const ChoiceOption& option) { return option.symbol == value; });
        case Kind::Int: {
            if (value.empty()) {
                return false;
            }
            char* end = nullptr;
            const long number = std::strtol(value.c_str(), &end, 10);
            if (*end != '\0') {
                return false;
            }
            if (item.hasRange && (number < item.rangeMin || number > item.rangeMax)) {
                return false;
            }
            break;
        }
        case Kind::String:
            if (value.find_first_of(forbiddenStringChars) != std::string::npos) {
                return false;
            }
            break;
    }
    return item.values.empty() || std::find(item.values.begin(), item.values.end(), value) != item.values.end();
}

std::string defaultValue(const std::vector<Item>& items, const Item& item) {
    for (const auto& entry : item.defaults) {
        if (!entry.condition.empty() && !ExpressionParser(items, entry.condition).evaluate()) {
            continue;
        }
        if (isValidValue(item, entry.value)) {
            return entry.value;
        }
    }
    switch (item.kind) {
        case Kind::Bool:
            return "n";
        case Kind::Choice:
            return item.options.front().symbol;
        case Kind::Int:
            if (!item.values.empty()) {
                return item.values.front();
            }
            return std::to_string(item.hasRange ? item.rangeMin : 0);
        case Kind::String:
            return item.values.empty() ? std::string() : item.values.front();
    }
    return {};
}

void applyDefaults(std::vector<Item>& items) {
    for (auto& item : items) {
        if (item.kind != Kind::Choice) {
            item.value.clear();
        }
    }
    for (auto& item : items) {
        if (item.kind != Kind::Choice || item.value.empty()) {
            item.value = defaultValue(items, item);
        }
    }
}

std::vector<Item> loadSettings(const Model& model, const std::string& configPath) {
    std::map<std::string, std::string> saved;
    std::ifstream input(configPath);
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.rfind("# CONFIG_", 0) == 0) {
            const auto marker = line.find(" is not set");
            if (marker != std::string::npos) {
                saved[line.substr(9, marker - 9)] = "n";
            }
            continue;
        }
        if (line.rfind("CONFIG_", 0) != 0) {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        std::string value = line.substr(separator + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        saved[line.substr(7, separator - 7)] = value;
    }

    // Items are resolved in Kconfig order so conditional defaults see the
    // board choice and any earlier symbols they depend on.
    std::vector<Item> items = model.items;
    for (auto& item : items) {
        item.value.clear();
    }
    for (auto& item : items) {
        bool resolved = false;
        if (item.kind == Kind::Choice) {
            for (const auto& option : item.options) {
                const auto found = saved.find(option.symbol);
                if (found != saved.end() && found->second == "y") {
                    item.value = option.symbol;
                    resolved = true;
                }
            }
        } else {
            const auto found = saved.find(item.symbol);
            if (found != saved.end() && isValidValue(item, found->second)) {
                item.value = found->second;
                resolved = true;
            }
        }
        if (!resolved) {
            item.value = defaultValue(items, item);
        }
    }
    return items;
}

bool saveSettings(const std::string& configPath, const std::vector<Item>& items, std::string& error) {
    const std::filesystem::path destination(configPath);
    std::error_code filesystemError;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), filesystemError);
        if (filesystemError) {
            error = filesystemError.message();
            return false;
        }
    }

    const std::string temporaryPath = configPath + ".tmp";
    {
        std::ofstream output(temporaryPath, std::ios::trunc);
        if (!output) {
            error = "cannot write " + temporaryPath;
            return false;
        }
        output << "#\n# audiox configuration\n# Generated from Kconfig; edit with make menuconfig, reset with make defconfig.\n#\n";
        std::string menu;
        for (const auto& item : items) {
            if (item.menu != menu) {
                menu = item.menu;
                output << "\n#\n# " << menu << "\n#\n";
            }
            if (item.kind == Kind::Choice) {
                for (const auto& option : item.options) {
                    if (option.symbol == item.value) {
                        output << "CONFIG_" << option.symbol << "=y\n";
                    } else {
                        output << "# CONFIG_" << option.symbol << " is not set\n";
                    }
                }
            } else if (item.kind == Kind::Bool && item.value == "n") {
                output << "# CONFIG_" << item.symbol << " is not set\n";
            } else if (item.kind == Kind::String) {
                output << "CONFIG_" << item.symbol << "=\"" << item.value << "\"\n";
            } else {
                output << "CONFIG_" << item.symbol << '=' << item.value << '\n';
            }
        }
        output.flush();
        if (!output) {
            error = "failed writing " + temporaryPath;
            return false;
        }
    }

    if (std::rename(temporaryPath.c_str(), configPath.c_str()) != 0) {
        error = "cannot replace " + configPath + ": " + std::strerror(errno);
        std::remove(temporaryPath.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> selectableValues(const Item& item) {
    if (item.kind == Kind::Choice) {
        std::vector<std::string> symbols;
        for (const auto& option : item.options) {
            symbols.push_back(option.symbol);
        }
        return symbols;
    }
    if (item.kind == Kind::Bool) {
        return {"n", "y"};
    }
    return item.values;
}

std::string valueLabel(const Item& item, const std::string& value) {
    if (item.kind == Kind::Choice) {
        for (const auto& option : item.options) {
            if (option.symbol == value) {
                return option.label;
            }
        }
    }
    if (item.kind == Kind::String && value.empty()) {
        return "(empty)";
    }
    return value;
}

// Changing one value (normally the board) re-evaluates conditional defaults;
// options still at their previous default follow the new one.
void setValue(std::vector<Item>& items, std::size_t index, const std::string& value) {
    std::vector<std::string> previousDefaults;
    for (const auto& item : items) {
        previousDefaults.push_back(defaultValue(items, item));
    }
    items[index].value = value;
    for (std::size_t other = 0; other < items.size(); ++other) {
        if (other == index || items[other].kind == Kind::Choice) {
            continue;
        }
        if (items[other].value == previousDefaults[other]) {
            items[other].value = defaultValue(items, items[other]);
        }
    }
}

std::string displayValue(const Item& item, std::size_t width) {
    if (item.kind == Kind::Bool) {
        return item.value == "y" ? "[*]" : "[ ]";
    }
    std::string text = valueLabel(item, item.value);
    if (width > 6 && text.size() > width - 4) {
        text = text.substr(0, width - 7) + "...";
    }
    return "< " + text + " >";
}

int drawWrapped(int y, int x, int width, int maxRow, const std::string& text) {
    int currentY = y;
    std::string line;
    std::string word;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == ' ') {
            if (!word.empty()) {
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (static_cast<int>(candidate.size()) > width && !line.empty()) {
                    if (currentY < maxRow) {
                        mvaddnstr(currentY, x, line.c_str(), width);
                    }
                    ++currentY;
                    line = word;
                } else {
                    line = candidate;
                }
                word.clear();
            }
            continue;
        }
        word.push_back(text[index]);
    }
    if (!line.empty()) {
        if (currentY < maxRow) {
            mvaddnstr(currentY, x, line.c_str(), width);
        }
        ++currentY;
    }
    return currentY;
}

struct Row {
    bool header;
    std::string text;
    std::size_t item;
};

std::vector<Row> buildRows(const std::vector<Item>& items) {
    std::vector<Row> rows;
    std::string menu = "\x01";
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index].menu != menu) {
            menu = items[index].menu;
            if (!rows.empty()) {
                rows.push_back({true, "", 0});
            }
            rows.push_back({true, menu.empty() ? "General" : menu, 0});
        }
        rows.push_back({false, {}, index});
    }
    return rows;
}

void drawScreen(const std::string& title, const std::vector<Item>& items, std::size_t selected,
                std::size_t& scroll, const std::string& status) {
    erase();
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);

    attron(COLOR_PAIR(1) | A_BOLD);
    mvhline(0, 0, ' ', columns);
    mvaddnstr(0, 2, title.c_str(), columns - 4);
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvaddnstr(1, 2, "Configure the build for the target board", columns - 4);
    mvhline(2, 0, ACS_HLINE, columns);

    if (rows < minimumRows || columns < minimumColumns) {
        mvprintw(4, 2, "Terminal is too small for menuconfig.");
        mvprintw(6, 2, "Resize to at least %d columns by %d rows, then retry.", minimumColumns, minimumRows);
        mvaddstr(rows - 2, 2, "Press q to exit");
        refresh();
        return;
    }

    const int leftPaneWidth = std::min(56, columns / 2 + 4);
    const int rightPaneX = leftPaneWidth + 2;
    const int rightWidth = columns - rightPaneX - 2;
    const int footerRow = rows - 1;
    const int listTop = 3;
    const int listHeight = rows - 3 - listTop;

    const auto layout = buildRows(items);
    std::size_t selectedRow = 0;
    for (std::size_t index = 0; index < layout.size(); ++index) {
        if (!layout[index].header && layout[index].item == selected) {
            selectedRow = index;
        }
    }
    if (selectedRow < scroll) {
        scroll = selectedRow > 0 && layout[selectedRow - 1].header ? selectedRow - 1 : selectedRow;
    } else if (selectedRow >= scroll + static_cast<std::size_t>(listHeight)) {
        scroll = selectedRow - static_cast<std::size_t>(listHeight) + 1;
    }

    for (int line = 0; line < listHeight && scroll + static_cast<std::size_t>(line) < layout.size(); ++line) {
        const Row& row = layout[scroll + static_cast<std::size_t>(line)];
        const int y = listTop + line;
        if (row.header) {
            attron(A_BOLD | COLOR_PAIR(1));
            mvaddnstr(y, 2, row.text.c_str(), leftPaneWidth - 3);
            attroff(A_BOLD | COLOR_PAIR(1));
            continue;
        }
        const Item& item = items[row.item];
        const bool active = row.item == selected;
        const std::string value = displayValue(item, static_cast<std::size_t>(leftPaneWidth / 2));
        const int labelWidth = leftPaneWidth - static_cast<int>(value.size()) - 7;
        if (active) {
            attron(A_REVERSE | A_BOLD);
            mvhline(y, 1, ' ', leftPaneWidth - 2);
        }
        mvaddch(y, 2, active ? '>' : ' ');
        mvaddnstr(y, 4, item.label.c_str(), labelWidth);
        mvaddstr(y, leftPaneWidth - static_cast<int>(value.size()) - 2, value.c_str());
        if (active) {
            attroff(A_REVERSE | A_BOLD);
        }
    }
    if (scroll > 0) {
        mvaddstr(listTop, leftPaneWidth - 3, "^");
    }
    if (scroll + static_cast<std::size_t>(listHeight) < layout.size()) {
        mvaddstr(listTop + listHeight - 1, leftPaneWidth - 3, "v");
    }

    const Item& item = items[selected];
    const int maxRow = rows - 3;
    mvvline(3, leftPaneWidth, ACS_VLINE, footerRow - 3);
    mvaddnstr(3, rightPaneX, item.label.c_str(), rightWidth);
    mvhline(4, rightPaneX, ACS_HLINE, rightWidth);
    int y = 5;
    if (item.kind != Kind::Choice) {
        attron(A_DIM);
        mvaddnstr(y++, rightPaneX, ("CONFIG_" + item.symbol).c_str(), rightWidth);
        attroff(A_DIM);
    }
    y = drawWrapped(y + 1, rightPaneX, rightWidth, maxRow, item.help) + 1;
    if (y < maxRow) {
        const std::string defaultText = "Default: " + valueLabel(item, defaultValue(items, item));
        y = drawWrapped(y, rightPaneX, rightWidth, maxRow, defaultText) + 1;
    }

    const auto choices = selectableValues(item);
    if (!choices.empty() && item.kind != Kind::Bool) {
        if (y < maxRow) {
            mvaddstr(y++, rightPaneX, "Available values:");
        }
        for (const auto& choice : choices) {
            if (y >= maxRow) {
                break;
            }
            const bool active = choice == item.value;
            if (active) {
                attron(A_BOLD | COLOR_PAIR(2));
            }
            mvaddstr(y, rightPaneX + 2, active ? "> " : "  ");
            mvaddnstr(y++, rightPaneX + 4, valueLabel(item, choice).c_str(), rightWidth - 4);
            if (active) {
                attroff(A_BOLD | COLOR_PAIR(2));
            }
        }
    } else if (item.kind != Kind::Bool && y < maxRow) {
        std::string hint = "Press Enter to edit.";
        if (item.hasRange) {
            hint += " Range " + std::to_string(item.rangeMin) + "-" + std::to_string(item.rangeMax) + ".";
        }
        y = drawWrapped(y, rightPaneX, rightWidth, maxRow, hint);
        if (y < maxRow) {
            y = drawWrapped(y, rightPaneX, rightWidth, maxRow, "Current: " + valueLabel(item, item.value));
        }
    }

    mvhline(rows - 3, 0, ACS_HLINE, columns);
    if (!status.empty()) {
        mvaddnstr(rows - 2, 2, status.c_str(), columns - 4);
    }
    attron(A_REVERSE);
    mvhline(footerRow, 0, ' ', columns);
    mvaddnstr(footerRow, 1, " Up/Down Select   Left/Right Change   Enter Edit   S Save   R Defaults   Q Exit   ? Help ",
              columns - 2);
    attroff(A_REVERSE);
    refresh();
}

int chooseValue(const Item& item) {
    const auto choices = selectableValues(item);
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);
    std::size_t longest = item.label.size();
    for (const auto& choice : choices) {
        longest = std::max(longest, valueLabel(item, choice).size());
    }
    const int height = std::min(static_cast<int>(choices.size()) + 4, rows - 2);
    const int width = std::min(static_cast<int>(longest) + 6, columns - 4);
    const int visible = height - 4;
    WINDOW* dialog = newwin(height, width, (rows - height) / 2, (columns - width) / 2);
    if (!dialog) {
        return -1;
    }
    keypad(dialog, TRUE);

    const auto current = std::find(choices.begin(), choices.end(), item.value);
    int choice = current == choices.end() ? 0 : static_cast<int>(current - choices.begin());
    const int count = static_cast<int>(choices.size());
    while (true) {
        werase(dialog);
        box(dialog, 0, 0);
        mvwaddnstr(dialog, 1, 2, item.label.c_str(), width - 4);
        const int first = std::max(0, std::min(choice - visible / 2, count - visible));
        for (int line = 0; line < visible && first + line < count; ++line) {
            const int index = first + line;
            if (index == choice) {
                wattron(dialog, A_REVERSE | A_BOLD);
            }
            mvwaddnstr(dialog, line + 2, 2, valueLabel(item, choices[static_cast<std::size_t>(index)]).c_str(), width - 4);
            if (index == choice) {
                wattroff(dialog, A_REVERSE | A_BOLD);
            }
        }
        wrefresh(dialog);
        const int key = wgetch(dialog);
        if (key == KEY_UP || key == 'k') {
            choice = (choice + count - 1) % count;
        } else if (key == KEY_DOWN || key == 'j') {
            choice = (choice + 1) % count;
        } else if (key == KEY_ENTER || key == '\n' || key == '\r') {
            delwin(dialog);
            return choice;
        } else if (key == 27 || key == 'q') {
            delwin(dialog);
            return -1;
        }
    }
}

bool editValue(const Item& item, std::string& result, std::string& status) {
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);
    const int width = std::min(columns - 4, 76);
    const int height = 7;
    WINDOW* dialog = newwin(height, width, (rows - height) / 2, (columns - width) / 2);
    if (!dialog) {
        return false;
    }
    keypad(dialog, TRUE);
    curs_set(1);

    std::string text = item.value;
    std::size_t cursor = text.size();
    const int fieldWidth = width - 4;
    std::string message = item.kind == Kind::Int ? "Enter a number." : "Enter text; quotes, \\, #, $ and ` are not allowed.";
    bool accepted = false;
    while (true) {
        werase(dialog);
        box(dialog, 0, 0);
        mvwaddnstr(dialog, 1, 2, item.label.c_str(), width - 4);
        mvwaddnstr(dialog, 5, 2, message.c_str(), width - 4);
        const std::size_t offset = cursor >= static_cast<std::size_t>(fieldWidth) ? cursor - static_cast<std::size_t>(fieldWidth) + 1 : 0;
        wattron(dialog, A_REVERSE);
        mvwhline(dialog, 3, 2, ' ', fieldWidth);
        mvwaddnstr(dialog, 3, 2, text.c_str() + offset, fieldWidth);
        wattroff(dialog, A_REVERSE);
        wmove(dialog, 3, 2 + static_cast<int>(cursor - offset));
        wrefresh(dialog);

        const int key = wgetch(dialog);
        if (key == 27) {
            break;
        }
        if (key == KEY_ENTER || key == '\n' || key == '\r') {
            const std::string candidate = item.kind == Kind::Int ? trim(text) : text;
            if (isValidValue(item, candidate)) {
                result = candidate;
                accepted = true;
                break;
            }
            message = item.hasRange ? "Invalid value; must be within " + std::to_string(item.rangeMin) + "-" +
                                          std::to_string(item.rangeMax) + "."
                                    : "Invalid value.";
        } else if (key == KEY_LEFT && cursor > 0) {
            --cursor;
        } else if (key == KEY_RIGHT && cursor < text.size()) {
            ++cursor;
        } else if (key == KEY_HOME || key == 1) {
            cursor = 0;
        } else if (key == KEY_END || key == 5) {
            cursor = text.size();
        } else if ((key == KEY_BACKSPACE || key == 127 || key == 8) && cursor > 0) {
            text.erase(--cursor, 1);
        } else if (key == KEY_DC && cursor < text.size()) {
            text.erase(cursor, 1);
        } else if (key == 21) {
            text.clear();
            cursor = 0;
        } else if (key >= 32 && key < 127) {
            text.insert(cursor++, 1, static_cast<char>(key));
        }
    }
    curs_set(0);
    delwin(dialog);
    if (!accepted) {
        status = "Edit cancelled.";
    }
    return accepted;
}

void showHelp() {
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);
    const int width = std::min(columns - 4, 66);
    const int height = 12;
    WINDOW* dialog = newwin(height, width, (rows - height) / 2, (columns - width) / 2);
    if (!dialog) {
        return;
    }
    box(dialog, 0, 0);
    mvwaddstr(dialog, 1, 2, "Menu controls");
    mvwaddstr(dialog, 3, 2, "Up/Down or PgUp/PgDn move through settings.");
    mvwaddstr(dialog, 4, 2, "Left/Right cycle a choice or toggle a boolean.");
    mvwaddstr(dialog, 5, 2, "Enter opens the selector or text editor.");
    mvwaddstr(dialog, 6, 2, "Changing the board updates options still at default.");
    mvwaddstr(dialog, 7, 2, "S saves .config. R resets options to board defaults.");
    mvwaddstr(dialog, 8, 2, "Q exits; unsaved changes prompt before leaving.");
    mvwaddstr(dialog, height - 2, 2, "Press any key to return");
    wrefresh(dialog);
    wgetch(dialog);
    delwin(dialog);
}

int confirmExit() {
    const std::string prompt = "Save changes before exiting?  y: save  n: discard  c: continue";
    const int row = LINES - 2;
    attron(A_REVERSE | A_BOLD);
    mvhline(row, 0, ' ', COLS);
    mvaddnstr(row, 2, prompt.c_str(), COLS - 4);
    attroff(A_REVERSE | A_BOLD);
    refresh();
    while (true) {
        const int key = getch();
        if (key == 'y' || key == 'Y') {
            return 'y';
        }
        if (key == 'n' || key == 'N') {
            return 'n';
        }
        if (key == 'c' || key == 'C' || key == 27) {
            return 'c';
        }
    }
}

void cycleValue(std::vector<Item>& items, std::size_t index, int direction) {
    Item& item = items[index];
    const auto choices = selectableValues(item);
    if (choices.empty()) {
        return;
    }
    const auto current = std::find(choices.begin(), choices.end(), item.value);
    int position = current == choices.end() ? 0 : static_cast<int>(current - choices.begin());
    const int count = static_cast<int>(choices.size());
    position = (position + direction + count) % count;
    setValue(items, index, choices[static_cast<std::size_t>(position)]);
}

bool hasChanges(const std::vector<Item>& items, const std::vector<Item>& savedItems) {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index].value != savedItems[index].value) {
            return true;
        }
    }
    return false;
}

int runMenu(const Model& model, const std::string& configPath) {
    std::vector<Item> items = loadSettings(model, configPath);
    auto savedItems = items;
    std::size_t selected = 0;
    std::size_t scroll = 0;
    std::string status;
    if (access(configPath.c_str(), F_OK) != 0) {
        status = "No existing configuration; showing defaults.";
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);
        init_pair(2, COLOR_GREEN, -1);
    }

    bool exitMenu = false;
    while (!exitMenu) {
        drawScreen(model.title, items, selected, scroll, status);
        const int key = getch();
        status.clear();
        switch (key) {
            case KEY_UP:
            case 'k':
                selected = (selected + items.size() - 1) % items.size();
                break;
            case KEY_DOWN:
            case 'j':
                selected = (selected + 1) % items.size();
                break;
            case KEY_PPAGE:
                selected = selected >= 5 ? selected - 5 : 0;
                break;
            case KEY_NPAGE:
                selected = std::min(items.size() - 1, selected + 5);
                break;
            case KEY_HOME:
                selected = 0;
                break;
            case KEY_END:
                selected = items.size() - 1;
                break;
            case KEY_LEFT:
            case KEY_RIGHT:
                cycleValue(items, selected, key == KEY_LEFT ? -1 : 1);
                break;
            case ' ':
            case KEY_ENTER:
            case '\n':
            case '\r': {
                Item& item = items[selected];
                if (item.kind == Kind::Bool) {
                    setValue(items, selected, item.value == "y" ? "n" : "y");
                } else if (!selectableValues(item).empty()) {
                    const int choice = chooseValue(item);
                    if (choice >= 0) {
                        setValue(items, selected, selectableValues(item)[static_cast<std::size_t>(choice)]);
                    }
                } else {
                    std::string value;
                    if (editValue(item, value, status)) {
                        setValue(items, selected, value);
                    }
                }
                break;
            }
            case 's':
            case 'S': {
                std::string error;
                if (saveSettings(configPath, items, error)) {
                    status = "Configuration saved to " + configPath;
                    savedItems = items;
                } else {
                    status = "Save failed: " + error;
                }
                break;
            }
            case 'r':
            case 'R':
                applyDefaults(items);
                status = "Options reset to the selected board's defaults. Press S to save.";
                break;
            case '?':
                showHelp();
                break;
            case 'q':
            case 'Q':
            case 27:
                if (hasChanges(items, savedItems)) {
                    const int exitChoice = confirmExit();
                    if (exitChoice == 'c') {
                        break;
                    }
                    if (exitChoice == 'y') {
                        std::string error;
                        if (!saveSettings(configPath, items, error)) {
                            status = "Save failed: " + error;
                            break;
                        }
                    }
                }
                exitMenu = true;
                break;
            default:
                break;
        }
    }

    endwin();
    return 0;
}

void usage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s CONFIG_FILE KCONFIG_FILE\n"
                 "       %s --defconfig KCONFIG_FILE CONFIG_FILE [CHOICE_SYMBOL]\n"
                 "       %s --olddefconfig KCONFIG_FILE CONFIG_FILE\n",
                 program, program, program);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool defconfig = mode == "--defconfig" && (argc == 4 || argc == 5);
    const bool olddefconfig = mode == "--olddefconfig" && argc == 4;
    if (!defconfig && !olddefconfig && argc != 3) {
        usage(argv[0]);
        return 2;
    }

    Model model;
    std::string error;
    if (!loadModel(argv[2], model, error)) {
        std::fprintf(stderr, "Kconfig error: %s\n", error.c_str());
        return 1;
    }

    if (defconfig || olddefconfig) {
        std::vector<Item> items;
        if (defconfig) {
            items = model.items;
            for (auto& item : items) {
                item.value.clear();
            }
            if (argc == 5) {
                bool found = false;
                for (auto& item : items) {
                    if (item.kind == Kind::Choice && isValidValue(item, argv[4])) {
                        item.value = argv[4];
                        found = true;
                    }
                }
                if (!found) {
                    std::fprintf(stderr, "defconfig failed: unknown choice symbol %s\n", argv[4]);
                    return 1;
                }
            }
            applyDefaults(items);
        } else {
            items = loadSettings(model, argv[3]);
        }
        if (!saveSettings(argv[3], items, error)) {
            std::fprintf(stderr, "%s failed: %s\n", mode.c_str() + 2, error.c_str());
            return 1;
        }
        return 0;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fprintf(stderr, "menuconfig requires an interactive terminal.\n");
        return 2;
    }

    return runMenu(model, argv[1]);
}
