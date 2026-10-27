#include "shared/ui/chat/Chat.h"

#include <windows.h>

#include <algorithm>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop::ui {

Chat& TheChat() {
    static Chat c;
    return c;
}

static constexpr size_t kMaxLines = 200;
static constexpr int kVisibleLines = 10;
static constexpr size_t kMaxInput = 160;
static constexpr uint64_t kFadeAfterMs = 30000;

void Chat::Add(const std::string& text, uint32_t argb) {
    LOGI("[chat] %s", text.c_str());
    m_lines.push_back({Widen(text), argb, NowMs()});
    while (m_lines.size() > kMaxLines) m_lines.pop_front();
}

void Chat::Open() {
    if (m_open) return;
    m_open = true;
    m_openTime = NowMs();
    m_input.clear();
    m_cursor = 0;
    m_histPos = -1;
    m_scroll = 0;
    In().SetKeyboardCapture(true);
}

void Chat::Close() {
    if (!m_open) return;
    m_open = false;
    m_scroll = 0;
    In().SetKeyboardCapture(false);
}

static std::wstring ReadClipboard() {
    std::wstring out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    for (auto& c : out)
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    return out;
}

void Chat::ProcessInput() {
    if (!m_open) return;
    auto& st = In().State();
    for (int vk : st.keyDowns) {
        switch (vk) {
            case VK_RETURN: {
                std::wstring text = m_input;
                Close();
                // trim
                while (!text.empty() && text.back() == L' ') text.pop_back();
                size_t start = text.find_first_not_of(L' ');
                if (start == std::wstring::npos) return;
                text = text.substr(start);
                if (m_history.empty() || m_history.back() != text) m_history.push_back(text);
                if (m_history.size() > 50) m_history.erase(m_history.begin());
                if (onSubmit) onSubmit(Narrow(text));
                return;
            }
            case VK_ESCAPE:
                Close();
                return;
            case VK_BACK:
                if (m_cursor > 0) {
                    m_input.erase(m_cursor - 1, 1);
                    --m_cursor;
                }
                break;
            case VK_DELETE:
                if (m_cursor < m_input.size()) m_input.erase(m_cursor, 1);
                break;
            case VK_LEFT:
                if (m_cursor > 0) --m_cursor;
                break;
            case VK_RIGHT:
                if (m_cursor < m_input.size()) ++m_cursor;
                break;
            case VK_HOME:
                m_cursor = 0;
                break;
            case VK_END:
                m_cursor = m_input.size();
                break;
            case VK_UP:
                if (!m_history.empty()) {
                    if (m_histPos < 0) m_histPos = (int)m_history.size();
                    if (m_histPos > 0) --m_histPos;
                    m_input = m_history[m_histPos];
                    m_cursor = m_input.size();
                }
                break;
            case VK_DOWN:
                if (m_histPos >= 0) {
                    ++m_histPos;
                    if (m_histPos >= (int)m_history.size()) {
                        m_histPos = -1;
                        m_input.clear();
                    } else {
                        m_input = m_history[m_histPos];
                    }
                    m_cursor = m_input.size();
                }
                break;
            case VK_PRIOR:
                m_scroll = std::min<int>(m_scroll + 5, std::max<int>(0, (int)m_lines.size() - kVisibleLines));
                break;
            case VK_NEXT:
                m_scroll = std::max(0, m_scroll - 5);
                break;
            case 'V':
                if (st.ctrl) {
                    std::wstring clip = ReadClipboard();
                    size_t room = kMaxInput > m_input.size() ? kMaxInput - m_input.size() : 0;
                    if (clip.size() > room) clip.resize(room);
                    m_input.insert(m_cursor, clip);
                    m_cursor += clip.size();
                }
                break;
            default:
                break;
        }
    }
    for (wchar_t c : st.chars) {
        if (st.ctrl) continue;  // Ctrl+V etc. produce control chars; already filtered
        if (m_input.size() >= kMaxInput) break;
        m_input.insert(m_input.begin() + (ptrdiff_t)m_cursor, c);
        ++m_cursor;
    }
    if (st.wheel) {
        m_scroll = std::clamp(m_scroll + st.wheel, 0, std::max<int>(0, (int)m_lines.size() - kVisibleLines));
    }
}

void Chat::Render() {
    auto& g = Gfx();
    if (!g.Ready()) return;
    float s = g.Scale();
    float x = 24 * s, y = 24 * s;
    float width = std::max(520.0f * s, g.Width() * 0.36f);
    float lh = g.LineHeight(FONT_NORMAL) * 0.95f;
    uint64_t now = NowMs();

    // collect wrapped lines from the end
    struct Vis {
        std::wstring text;
        uint32_t color;
        float alpha;
    };
    std::vector<Vis> vis;
    int skip = m_scroll;
    for (auto it = m_lines.rbegin(); it != m_lines.rend() && (int)vis.size() < kVisibleLines; ++it) {
        float alpha = 1.0f;
        if (!m_open) {
            uint64_t age = now - it->time;
            if (age > kFadeAfterMs + 2000) break;
            if (age > kFadeAfterMs) alpha = 1.0f - (float)(age - kFadeAfterMs) / 2000.0f;
        }
        auto wrapped = g.Wrap(FONT_NORMAL, it->text, width);
        for (auto w = wrapped.rbegin(); w != wrapped.rend() && (int)vis.size() < kVisibleLines; ++w) {
            if (skip > 0) {
                --skip;
                continue;
            }
            vis.push_back({*w, it->color, alpha});
        }
    }
    if (m_open) {
        g.Rect(x - 8 * s, y - 6 * s, width + 16 * s, lh * kVisibleLines + 12 * s, 0x60000000);
    }
    float cy = y + lh * (kVisibleLines - 1);
    for (auto& v : vis) {
        g.TextW(FONT_NORMAL, x, cy, v.text, WithAlpha(v.color, v.alpha), TEXT_OUTLINE);
        cy -= lh;
    }
    if (m_open) {
        float iy = y + lh * kVisibleLines + 10 * s;
        float ih = lh + 8 * s;
        g.Rect(x - 8 * s, iy, width + 16 * s, ih, 0xB0101010);
        g.RectOutline(x - 8 * s, iy, width + 16 * s, ih, 0xFF5A82C8);
        const wchar_t* prefix = TR(L"Сказать: ", L"Say: ");
        std::wstring shown = prefix + m_input;
        float prefixW = g.TextWidthW(FONT_NORMAL, prefix);
        // scroll the input horizontally if it is too long
        float caretX = prefixW + g.TextWidthW(FONT_NORMAL, std::wstring_view(m_input).substr(0, m_cursor));
        float offset = std::max(0.0f, caretX - width + 10 * s);
        g.TextW(FONT_NORMAL, x - offset, iy + 4 * s, shown, 0xFFFFFFFF, TEXT_SHADOW);
        if (((now - m_openTime) / 500) % 2 == 0) g.Rect(x - offset + caretX, iy + 5 * s, 2 * s, lh - 2 * s, 0xFFFFFFFF);
        if (m_scroll > 0)
            g.Text(FONT_SMALL, x + width, y - 4 * s, Format("▲ %d", m_scroll), 0xFFB0B0B0, TEXT_RIGHT | TEXT_SHADOW);
    }
}

}  // namespace coop::ui
