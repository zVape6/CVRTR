// SPDX-License-Identifier: GPL-3.0-only
#include <GLFW/glfw3.h>
#include <windows.h>
#include "window_chrome.h"
#include <iostream>

int main() {
    if(!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED,GLFW_FALSE);
    GLFWwindow* window=glfwCreateWindow(1000,760,"CVRTR chrome test",nullptr,nullptr);
    if(!window) {glfwTerminate();return 1;}
    glfwSetWindowSizeLimits(window,1000,760,GLFW_DONT_CARE,GLFW_DONT_CARE);
    chrome::Install(window);
    HWND hwnd=glfwGetWin32Window(window);
    bool ok=true;
    auto check=[&](bool condition,const char* label) {if(!condition) {std::cerr<<label<<'\n';ok=false;}};
    auto hit=[&](int x,int y) {
        POINT p{x,y};ClientToScreen(hwnd,&p);
        return SendMessageW(hwnd,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y));
    };
    check(hit(200,40)==HTCAPTION,"Header must allow native dragging and double click");
    check(hit(40,40)==HTCAPTION,"Former sidebar control area must allow dragging");
    check(hit(970,18)==HTCLIENT,"Raised window controls must remain clickable");
    check(hit(970,70)==HTCAPTION,"Logo must allow dragging below the controls");
    check(hit(160,130)==HTCLIENT,"Sidebar toggle must remain clickable on the panel");
    check(hit(2,200)==HTLEFT,"Left edge must resize");
    check(hit(2,2)==HTTOPLEFT,"Corner must resize");
    check(hit(500,400)==HTCLIENT,"Workspace must remain interactive");
    MINMAXINFO limits{};SendMessageW(hwnd,WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&limits));
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
    check(limits.ptMaxSize.x==monitor.rcWork.right-monitor.rcWork.left &&
        limits.ptMaxSize.y==monitor.rcWork.bottom-monitor.rcWork.top,"Maximized bounds must respect taskbar");
    RECT client{},outer{};GetClientRect(hwnd,&client);GetWindowRect(hwnd,&outer);
    check(client.right==outer.right-outer.left && client.bottom==outer.bottom-outer.top,"System title bar must be absent");
    glfwDestroyWindow(window);glfwTerminate();
    return ok?0:1;
}
