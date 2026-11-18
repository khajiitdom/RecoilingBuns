// SMF.h - minimal binding to SKSE Menu Framework 3.x (the F11 menu), resolved at run time with GetProcAddress.
// No import library, no hard dependency: without the framework every call is a no-op and AddSectionItem returns false.
//
// Declarations and signatures follow SKSEMenuFramework.h from
// https://github.com/Thiago099/SKSE-Menu-Framework-3-Example (commit f21b8e034c, 2026-09-03),
// MIT License, Copyright (c) 2024 Thiago Kaique:
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to the following conditions:
//   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
//   the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
//   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
//   COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Every name below is in the export table of SKSEMenuFramework.dll 3.18 (Nexus 120352; file version 3.14.0.0).
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>

namespace SMF
{
    // Not cached: our plugin loads before SKSEMenuFramework.dll ("B"/"R" < "S"), so an early lookup must not stick.
    inline HMODULE Module() { return GetModuleHandleW(L"SKSEMenuFramework"); }

    template <class T>
    T Fn(const char* a_name)
    {
        const HMODULE m = Module();
        return m ? reinterpret_cast<T>(GetProcAddress(m, a_name)) : nullptr;
    }

    // x64 ignores __stdcall; it is kept so the types match the framework's own typedefs.
    using RenderFunction = void(__stdcall*)();
    enum EventType : int { kNone = 0, kOpenMenu = 1, kCloseMenu = 2, kBeforeRender = 3, kAfterRender = 4 };
    using EventCallback = void(__stdcall*)(EventType);

    struct WindowInterface   // layout owned by the framework
    {
        std::atomic<bool> IsOpen{ false };
        std::atomic<bool> BlockUserInput{ true };
    };

    // True once the framework DLL is loaded (call at kPostLoad or later).
    inline bool IsLoaded() { return Fn<void*>("AddSectionItem") != nullptr; }

    // Full path, '/' separated ("Recoiling Buns/Sandbox"); "\\/" is a literal slash. False when the framework is absent.
    inline bool AddSectionItem(const char* a_path, RenderFunction a_render)
    {
        const auto f = Fn<void (*)(const char*, RenderFunction)>("AddSectionItem");
        if (!f) return false;
        f(a_path, a_render);
        return true;
    }

    inline WindowInterface* GetMainWindow()
    {
        const auto f = Fn<WindowInterface* (*)()>("GetMainWindow");
        return f ? f() : nullptr;
    }

    // kOpenMenu / kCloseMenu / kBeforeRender / kAfterRender, on the main thread. Returns 0 when absent.
    inline std::int64_t RegisterEvent(EventCallback a_cb, float a_priority = 0.0f)
    {
        const auto f = Fn<std::int64_t (*)(EventCallback, float)>("RegisterEventPriority");
        return f ? f(a_cb, a_priority) : 0;
    }

    inline void UnregisterEvent(std::int64_t a_id)
    {
        if (const auto f = Fn<void (*)(std::int64_t)>("UnregisterEvent"); f && a_id) f(a_id);
    }

    // ------------------------------------------------------------------------------------------------ ImGui (cimgui ABI)
    // Call these only from inside a framework render callback (the framework is loaded and an ImGui frame is open).
    namespace ImGui
    {
        struct ImVec2
        {
            float x, y;
        };
        using ImGuiSliderFlags = int;
        using ImGuiTreeNodeFlags = int;
        using ImGuiColorEditFlags = int;
        using ImGuiInputTextFlags = int;
        enum : int {
            ImGuiSliderFlags_AlwaysClamp = 1 << 4,
            ImGuiSliderFlags_Logarithmic = 1 << 5,
            ImGuiTreeNodeFlags_DefaultOpen = 1 << 5,
            ImGuiColorEditFlags_NoInputs = 1 << 5,
            ImGuiColorEditFlags_Float = 1 << 24,
        };

#define SMF_IG(type, name) \
    static const auto f = ::SMF::Fn<type>(name)

        inline void TextV(const char* a_fmt, va_list a_args)
        {
            SMF_IG(void (*)(const char*, va_list), "igTextV");
            if (f) f(a_fmt, a_args);
        }
        inline void Text(const char* a_fmt, ...)   // printf format: pass user strings as Text("%s", s)
        {
            va_list a;
            va_start(a, a_fmt);
            TextV(a_fmt, a);
            va_end(a);
        }
        inline void TextWrapped(const char* a_fmt, ...)
        {
            SMF_IG(void (*)(const char*, va_list), "igTextWrappedV");
            va_list a;
            va_start(a, a_fmt);
            if (f) f(a_fmt, a);
            va_end(a);
        }
        inline void TextDisabled(const char* a_fmt, ...)
        {
            SMF_IG(void (*)(const char*, va_list), "igTextDisabledV");
            va_list a;
            va_start(a, a_fmt);
            if (f) f(a_fmt, a);
            va_end(a);
        }
        inline void SetItemTooltip(const char* a_fmt, ...)
        {
            SMF_IG(void (*)(const char*, va_list), "igSetItemTooltipV");
            va_list a;
            va_start(a, a_fmt);
            if (f) f(a_fmt, a);
            va_end(a);
        }
        inline void TextUnformatted(const char* a_text, const char* a_end = nullptr)
        {
            SMF_IG(void (*)(const char*, const char*), "igTextUnformatted");
            if (f) f(a_text, a_end);
        }
        inline void SeparatorText(const char* a_label)
        {
            SMF_IG(void (*)(const char*), "igSeparatorText");
            if (f) f(a_label);
        }
        inline void Separator()
        {
            SMF_IG(void (*)(), "igSeparator");
            if (f) f();
        }
        inline void Spacing()
        {
            SMF_IG(void (*)(), "igSpacing");
            if (f) f();
        }
        inline void SameLine(float a_offsetFromStartX = 0.0f, float a_spacing = -1.0f)
        {
            SMF_IG(void (*)(float, float), "igSameLine");
            if (f) f(a_offsetFromStartX, a_spacing);
        }
        inline void Indent(float a_w = 0.0f)
        {
            SMF_IG(void (*)(float), "igIndent");
            if (f) f(a_w);
        }
        inline void Unindent(float a_w = 0.0f)
        {
            SMF_IG(void (*)(float), "igUnindent");
            if (f) f(a_w);
        }
        inline void SetNextItemWidth(float a_w)
        {
            SMF_IG(void (*)(float), "igSetNextItemWidth");
            if (f) f(a_w);
        }
        inline bool Button(const char* a_label, ImVec2 a_size = { 0.0f, 0.0f })
        {
            SMF_IG(bool (*)(const char*, ImVec2), "igButton");
            return f && f(a_label, a_size);
        }
        inline bool Checkbox(const char* a_label, bool* a_v)
        {
            SMF_IG(bool (*)(const char*, bool*), "igCheckbox");
            return f && f(a_label, a_v);
        }
        inline bool SliderFloat(const char* a_label, float* a_v, float a_min, float a_max, const char* a_fmt = "%.3f", ImGuiSliderFlags a_flags = 0)
        {
            SMF_IG(bool (*)(const char*, float*, float, float, const char*, ImGuiSliderFlags), "igSliderFloat");
            return f && f(a_label, a_v, a_min, a_max, a_fmt, a_flags);
        }
        inline bool SliderInt(const char* a_label, int* a_v, int a_min, int a_max, const char* a_fmt = "%d", ImGuiSliderFlags a_flags = 0)
        {
            SMF_IG(bool (*)(const char*, int*, int, int, const char*, ImGuiSliderFlags), "igSliderInt");
            return f && f(a_label, a_v, a_min, a_max, a_fmt, a_flags);
        }
        inline bool DragFloat(const char* a_label, float* a_v, float a_speed = 1.0f, float a_min = 0.0f, float a_max = 0.0f, const char* a_fmt = "%.3f",
            ImGuiSliderFlags a_flags = 0)
        {
            SMF_IG(bool (*)(const char*, float*, float, float, float, const char*, ImGuiSliderFlags), "igDragFloat");
            return f && f(a_label, a_v, a_speed, a_min, a_max, a_fmt, a_flags);
        }
        inline bool InputFloat(const char* a_label, float* a_v, float a_step = 0.0f, float a_stepFast = 0.0f, const char* a_fmt = "%.3f",
            ImGuiInputTextFlags a_flags = 0)
        {
            SMF_IG(bool (*)(const char*, float*, float, float, const char*, ImGuiInputTextFlags), "igInputFloat");
            return f && f(a_label, a_v, a_step, a_stepFast, a_fmt, a_flags);
        }
        inline bool Combo(const char* a_label, int* a_current, const char* const a_items[], int a_count, int a_popupMaxItems = -1)
        {
            SMF_IG(bool (*)(const char*, int*, const char* const[], int, int), "igCombo_Str_arr");
            return f && f(a_label, a_current, a_items, a_count, a_popupMaxItems);
        }
        inline bool ColorEdit3(const char* a_label, float a_col[3], ImGuiColorEditFlags a_flags = 0)
        {
            SMF_IG(bool (*)(const char*, float*, ImGuiColorEditFlags), "igColorEdit3");
            return f && f(a_label, a_col, a_flags);
        }
        inline bool CollapsingHeader(const char* a_label, ImGuiTreeNodeFlags a_flags = 0)
        {
            SMF_IG(bool (*)(const char*, ImGuiTreeNodeFlags), "igCollapsingHeader_TreeNodeFlags");
            return f && f(a_label, a_flags);
        }
        inline bool TreeNode(const char* a_label)   // when true, call TreePop() after the children
        {
            SMF_IG(bool (*)(const char*), "igTreeNode_Str");
            return f && f(a_label);
        }
        inline void TreePop()
        {
            SMF_IG(void (*)(), "igTreePop");
            if (f) f();
        }
        inline void PushID(const char* a_id)
        {
            SMF_IG(void (*)(const char*), "igPushID_Str");
            if (f) f(a_id);
        }
        inline void PushID(int a_id)
        {
            SMF_IG(void (*)(int), "igPushID_Int");
            if (f) f(a_id);
        }
        inline void PopID()
        {
            SMF_IG(void (*)(), "igPopID");
            if (f) f();
        }
        inline void BeginDisabled(bool a_disabled = true)
        {
            SMF_IG(void (*)(bool), "igBeginDisabled");
            if (f) f(a_disabled);
        }
        inline void EndDisabled()
        {
            SMF_IG(void (*)(), "igEndDisabled");
            if (f) f();
        }
        inline bool IsItemDeactivatedAfterEdit()   // true on the frame a slider is released after a change
        {
            SMF_IG(bool (*)(), "igIsItemDeactivatedAfterEdit");
            return f && f();
        }

#undef SMF_IG
    }
}
