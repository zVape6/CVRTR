// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windowsx.h>

namespace chrome {
inline WNDPROC original = nullptr;
inline LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCALCSIZE && wp) return 0;
    if (message == WM_GETMINMAXINFO) {
        LRESULT result = CallWindowProcW(original, hwnd, message, wp, lp);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMaxPosition = {monitor.rcWork.left-monitor.rcMonitor.left, monitor.rcWork.top-monitor.rcMonitor.top};
            limits->ptMaxSize = {monitor.rcWork.right-monitor.rcWork.left, monitor.rcWork.bottom-monitor.rcWork.top};
        }
        return result;
    }
    if (message == WM_NCHITTEST) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &point);
        RECT rect; GetClientRect(hwnd, &rect);
        const int edge = 6;
        if (!IsZoomed(hwnd)) {
            bool left=point.x<edge, right=point.x>=rect.right-edge;
            bool top=point.y<edge, bottom=point.y>=rect.bottom-edge;
            if(top && left) return HTTOPLEFT; if(top && right) return HTTOPRIGHT;
            if(bottom && left) return HTBOTTOMLEFT; if(bottom && right) return HTBOTTOMRIGHT;
            if(left) return HTLEFT; if(right) return HTRIGHT; if(top) return HTTOP; if(bottom) return HTBOTTOM;
        }
        // The empty header and logo retain native dragging, double-click and snapping.
        bool controls=point.x>=rect.right-160 && point.y<34;
        if(point.y>=8 && point.y<108 && !controls) return HTCAPTION;
        return HTCLIENT;
    }
    return CallWindowProcW(original, hwnd, message, wp, lp);
}
inline void Install(GLFWwindow* window) {
    HWND hwnd = glfwGetWin32Window(window);
    original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Procedure)));
    LONG_PTR style=GetWindowLongPtrW(hwnd,GWL_STYLE);
    SetWindowLongPtrW(hwnd,GWL_STYLE,(style & ~WS_CAPTION) | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
}
#endif
