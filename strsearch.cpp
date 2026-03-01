#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <set>
#include <map>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

#define ID_EDIT_DIR       101
#define ID_BTN_BROWSE     102
#define ID_EDIT_KEYWORDS  103
#define ID_BTN_SEARCH     105
#define ID_BTN_CLEAR      106
#define ID_EDIT_RESULT    107
#define ID_LABEL_STATUS   108
#define ID_EDIT_CTX       109
#define ID_EDIT_EXT       110
#define ID_BTN_COPY       111
#define ID_EDIT_MAXHIT    112
#define ID_CHK_RECURSE    113

#define CLR_BG       RGB(245,247,250)
#define CLR_PANEL    RGB(255,255,255)
#define CLR_ACCENT   RGB(0,120,215)
#define CLR_ACCENT_H RGB(0,102,184)
#define CLR_BTN_CLR  RGB(108,117,125)
#define CLR_BTN_CPY  RGB(40,167,69)
#define CLR_TEXT     RGB(33,37,41)
#define CLR_BORDER   RGB(206,212,218)

HWND hEditDir, hEditKeywords, hEditResult, hLabelStatus;
HWND hEditCtx, hEditExt, hEditMaxHit, hChkRecurse;
HWND hBtnSearch, hBtnClear, hBtnCopy, hBtnBrowse;
HFONT hFontUI, hFontMono, hFontLabel;
HBRUSH hBrushBg, hBrushPanel;

const wchar_t* DEFAULT_EXTS =
    L".py .txt .md .cpp .c .h .hpp .cs .java .js .ts .jsx .tsx "
    L".html .htm .css .scss .xml .json .yaml .yml .toml .ini "
    L".cfg .conf .env .bat .cmd .sh .ps1 .sql .r .go .rs "
    L".kt .lua .rb .php .log .csv";

// ── 布局常量（行 Y 坐标） ──────────────────────────
// Row0: 目录           y=10  h=24
// Row1: 参数行         y=44  h=24
// Row2: 文件类型标签   y=78  h=18
// Row3: 文件类型框     y=96  h=24
// Row4: 关键词标签     y=130 h=18
// Row5: 关键词框       y=148 h=72
// Row6: 按钮           y=230 h=30
// Row7: 结果           y=270
#define Y0   10
#define Y1   44
#define Y2   78
#define Y3   96
#define Y4  130
#define Y5  148
#define Y6  230
#define Y7  270
#define LH   24   // line height
#define LBH  18   // label height

struct SearchParams {
    std::wstring rootDir;
    std::vector<std::wstring> keywords;
    std::set<std::wstring> exts;
    int contextLines, maxHitPerKw;
    bool recurse;
};

void AppendResult(const std::wstring& s) {
    int n = GetWindowTextLength(hEditResult);
    SendMessage(hEditResult, EM_SETSEL, n, n);
    SendMessage(hEditResult, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
}

bool IsTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    char buf[512]; auto n = f.read(buf,512).gcount();
    for (int i=0;i<n;i++) if(!buf[i]) return false;
    return true;
}

std::wstring ToWStr(const std::string& raw) {
    int sz=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,raw.c_str(),(int)raw.size(),0,0);
    if(sz>0){std::wstring w(sz,0);MultiByteToWideChar(CP_UTF8,0,raw.c_str(),(int)raw.size(),&w[0],sz);return w;}
    sz=MultiByteToWideChar(CP_ACP,0,raw.c_str(),(int)raw.size(),0,0);
    if(sz<=0) return L"";
    std::wstring w(sz,0);MultiByteToWideChar(CP_ACP,0,raw.c_str(),(int)raw.size(),&w[0],sz);return w;
}

bool ReadLines(const fs::path& path, std::vector<std::wstring>& lines) {
    std::ifstream f(path,std::ios::binary);if(!f)return false;
    std::string raw((std::istreambuf_iterator<char>(f)),{});
    if(raw.size()>=3&&(unsigned char)raw[0]==0xEF&&(unsigned char)raw[1]==0xBB&&(unsigned char)raw[2]==0xBF)
        raw=raw.substr(3);
    std::wistringstream ss(ToWStr(raw));std::wstring ln;
    while(std::getline(ss,ln)){if(!ln.empty()&&ln.back()==L'\r')ln.pop_back();lines.push_back(ln);}
    return true;
}

std::wstring Trim(std::wstring s){
    const std::wstring w=L" \t\r\n";
    s.erase(0,s.find_first_not_of(w));
    auto p=s.find_last_not_of(w);
    if(p!=std::wstring::npos)s.erase(p+1);else s.clear();return s;}

struct HitRecord { std::wstring file; int lineNo,hitIdx; std::vector<std::wstring> ctx; };
struct ThreadData { SearchParams p; HWND hWnd; };

DWORD WINAPI SearchThread(LPVOID lp){
    auto* td=(ThreadData*)lp;
    auto& p=td->p;
    SetWindowText(hEditResult,L"");
    std::map<std::wstring,std::vector<HitRecord>> hits;
    std::map<std::wstring,int> total;
    for(auto& k:p.keywords){hits[k]={};total[k]=0;}
    int files=0;

    auto proc=[&](const fs::path& fp){
        std::wstring ext=fp.extension().wstring();
        std::transform(ext.begin(),ext.end(),ext.begin(),::towlower);
        bool ok=p.exts.empty()?IsTextFile(fp):(p.exts.count(ext)>0);
        if(!ok)return;
        std::vector<std::wstring> lines;
        if(!ReadLines(fp,lines))return;
        files++;
        for(int i=0;i<(int)lines.size();i++){
            for(auto& k:p.keywords){
                if(lines[i].find(k)==std::wstring::npos)continue;
                total[k]++;
                if((int)hits[k].size()>=p.maxHitPerKw)continue;
                HitRecord r; r.file=fp.wstring(); r.lineNo=i+1;
                int s=std::max(0,i-p.contextLines),e=std::min((int)lines.size(),i+p.contextLines+1);
                r.hitIdx=i-s;
                for(int j=s;j<e;j++)r.ctx.push_back(lines[j]);
                hits[k].push_back(r);
            }
        }
    };

    try{
        if(p.recurse)
            for(auto& e:fs::recursive_directory_iterator(p.rootDir,fs::directory_options::skip_permission_denied))
                {if(e.is_regular_file())proc(e.path());}
        else
            for(auto& e:fs::directory_iterator(p.rootDir,fs::directory_options::skip_permission_denied))
                {if(e.is_regular_file())proc(e.path());}
    }catch(...){}

    AppendResult(L"目录: "+p.rootDir+L"\r\n");
    AppendResult(std::wstring(p.recurse?L"模式: 含子文件夹":L"模式: 仅当前文件夹")+L"\r\n");
    AppendResult(std::wstring(60,L'=')+L"\r\n\r\n");
    int shown=0;
    for(auto& k:p.keywords){
        auto& rv=hits[k]; int tot=total[k]; shown+=(int)rv.size();
        std::wstring hdr=L"【关键词】 "+k+L"   共 "+std::to_wstring(tot)+L" 处匹配";
        if(tot>p.maxHitPerKw) hdr+=L"，显示前 "+std::to_wstring((int)rv.size())+L" 条";
        AppendResult(hdr+L"\r\n"+std::wstring(55,L'-')+L"\r\n");
        if(rv.empty()){AppendResult(L"  （无匹配）\r\n\r\n");continue;}
        for(int r=0;r<(int)rv.size();r++){
            auto& rec=rv[r];
            AppendResult(L"  ["+std::to_wstring(r+1)+L"] "+rec.file+L"  第 "+std::to_wstring(rec.lineNo)+L" 行\r\n");
            int sl=rec.lineNo-rec.hitIdx;
            for(int j=0;j<(int)rec.ctx.size();j++){
                AppendResult((j==rec.hitIdx?L"  => ":L"     ")+std::to_wstring(sl+j)+L"  "+rec.ctx[j]+L"\r\n");
            }
            AppendResult(L"\r\n");
        }
        AppendResult(L"\r\n");
    }
    AppendResult(std::wstring(60,L'=')+L"\r\n");
    std::wstring sum=L"完成  |  扫描 "+std::to_wstring(files)+L" 个文件  |  显示 "+std::to_wstring(shown)+L" 条结果";
    AppendResult(sum+L"\r\n");
    SetWindowText(hLabelStatus,sum.c_str());
    EnableWindow(hBtnSearch,TRUE);EnableWindow(hBtnCopy,TRUE);
    delete td;return 0;
}

std::set<std::wstring> ParseExts(const std::wstring& raw){
    std::set<std::wstring> r;std::wistringstream ss(raw);std::wstring t;
    while(ss>>t){std::transform(t.begin(),t.end(),t.begin(),::towlower);
        if(!t.empty()&&t[0]!=L'.')t=L"."+t;r.insert(t);}return r;}

void BrowseDir(HWND hw){
    BROWSEINFOW bi={};bi.hwndOwner=hw;bi.lpszTitle=L"选择搜索目录";bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST p=SHBrowseForFolderW(&bi);
    if(p){wchar_t path[MAX_PATH];if(SHGetPathFromIDListW(p,path))SetWindowText(hEditDir,path);CoTaskMemFree(p);}
}

void StartSearch(HWND hw){
    wchar_t db[MAX_PATH]={};GetWindowText(hEditDir,db,MAX_PATH);
    wchar_t cb[8]={},mb[8]={};
    GetWindowText(hEditCtx,cb,8);GetWindowText(hEditMaxHit,mb,8);
    int ctx=std::max(0,std::min(10,_wtoi(cb)));
    int mx =std::max(1,std::min(9999,_wtoi(mb)));
    bool rec=(SendMessage(hChkRecurse,BM_GETCHECK,0,0)==BST_CHECKED);

    std::vector<std::wstring> kws;
    {int n=GetWindowTextLength(hEditKeywords);
     if(n>0){std::wstring r(n,0);GetWindowText(hEditKeywords,&r[0],n+1);
      std::wistringstream ss(r);std::wstring k;
      while(std::getline(ss,k)){k=Trim(k);if(!k.empty())kws.push_back(k);}}}
    if(kws.empty()){MessageBox(hw,L"请输入至少一个关键词！",L"提示",MB_OK|MB_ICONWARNING);return;}

    int en=GetWindowTextLength(hEditExt);
    std::wstring er(en?en:1,0);if(en>0)GetWindowText(hEditExt,&er[0],en+1);

    auto* td=new ThreadData();
    td->p.rootDir=db;td->p.keywords=kws;td->p.contextLines=ctx;
    td->p.maxHitPerKw=mx;td->p.recurse=rec;td->p.exts=ParseExts(er);td->hWnd=hw;
    EnableWindow(hBtnSearch,FALSE);EnableWindow(hBtnCopy,FALSE);
    SetWindowText(hLabelStatus,L"搜索中，请稍候...");
    HANDLE h=CreateThread(NULL,0,SearchThread,td,0,NULL);CloseHandle(h);
}

void CopyResults(){
    int n=GetWindowTextLength(hEditResult);if(!n)return;
    std::wstring b(n+1,0);GetWindowText(hEditResult,&b[0],n+1);
    if(OpenClipboard(NULL)){EmptyClipboard();
     HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,(n+1)*sizeof(wchar_t));
     if(hg){memcpy(GlobalLock(hg),b.c_str(),(n+1)*sizeof(wchar_t));GlobalUnlock(hg);SetClipboardData(CF_UNICODETEXT,hg);}
     CloseClipboard();SetWindowText(hLabelStatus,L"√ 已复制到剪贴板");}
}

void DrawBtn(DRAWITEMSTRUCT* di,COLORREF bg,COLORREF bgh,const wchar_t* t){
    HDC dc=di->hDC;RECT rc=di->rcItem;
    bool hv=(di->itemState&ODS_HOTLIGHT)!=0,pr=(di->itemState&ODS_SELECTED)!=0,ds=(di->itemState&ODS_DISABLED)!=0;
    HBRUSH br=CreateSolidBrush(ds?CLR_BORDER:(pr||hv?bgh:bg));
    HRGN rg=CreateRoundRectRgn(0,0,rc.right-rc.left+1,rc.bottom-rc.top+1,8,8);
    SelectClipRgn(dc,rg);FillRect(dc,&rc,br);SelectClipRgn(dc,NULL);
    DeleteObject(rg);DeleteObject(br);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ds?RGB(160,160,160):RGB(255,255,255));
    SelectObject(dc,hFontUI);DrawText(dc,t,-1,&rc,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}

LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        hFontUI   =CreateFont(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        hFontMono =CreateFont(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas");
        hFontLabel=CreateFont(13,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        hBrushBg  =CreateSolidBrush(CLR_BG);
        hBrushPanel=CreateSolidBrush(CLR_PANEL);

        // helpers
        auto mkL=[&](const wchar_t* t,int x,int y,int w,int h){
            HWND h2=CreateWindow(L"STATIC",t,WS_CHILD|WS_VISIBLE,x,y,w,h,hw,NULL,NULL,NULL);
            SendMessage(h2,WM_SETFONT,(WPARAM)hFontLabel,TRUE);return h2;};
        auto mkE=[&](const wchar_t* t,int x,int y,int w,int h,HMENU id,bool multi=false)->HWND{
            DWORD s=WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL|(multi?ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL:0);
            HWND h2=CreateWindow(L"EDIT",t,s,x,y,w,h,hw,id,NULL,NULL);
            SendMessage(h2,WM_SETFONT,(WPARAM)hFontMono,TRUE);return h2;};
        auto mkB=[&](const wchar_t* t,int x,int y,int w,int h,HMENU id)->HWND{
            HWND h2=CreateWindow(L"BUTTON",t,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,x,y,w,h,hw,id,NULL,NULL);
            SendMessage(h2,WM_SETFONT,(WPARAM)hFontUI,TRUE);return h2;};

        // Row 0: 目录
        mkL(L"搜索目录",10,Y0+4,70,LBH);
        hEditDir  =mkE(L".",84,Y0,0,LH,(HMENU)ID_EDIT_DIR);   // width set in WM_SIZE
        hBtnBrowse=mkB(L"浏览",0,Y0,60,LH,(HMENU)ID_BTN_BROWSE); // x set in WM_SIZE

        // Row 1: 参数行（上下文 | 每词最多 | 子文件夹）
        mkL(L"上下文行数",10,Y1+4,80,LBH);
        hEditCtx   =mkE(L"2",   95,Y1,40,LH,(HMENU)ID_EDIT_CTX);
        mkL(L"每词最多条数",145,Y1+4,95,LBH);
        hEditMaxHit=mkE(L"10", 245,Y1,50,LH,(HMENU)ID_EDIT_MAXHIT);
        hChkRecurse=CreateWindow(L"BUTTON",L"包含子文件夹",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                    305,Y1+2,130,LH,hw,(HMENU)ID_CHK_RECURSE,NULL,NULL);
        SendMessage(hChkRecurse,WM_SETFONT,(WPARAM)hFontUI,TRUE);
        SendMessage(hChkRecurse,BM_SETCHECK,BST_UNCHECKED,0);

        // Row 2-3: 文件类型
        mkL(L"文件类型（空格分隔，清空 = 自动检测所有文本文件）",10,Y2,330,LBH);
        hEditExt   =mkE(DEFAULT_EXTS,10,Y3,0,LH,(HMENU)ID_EDIT_EXT); // width in WM_SIZE

        // Row 4-5: 关键词
        mkL(L"搜索关键词（每行一个）",10,Y4,170,LBH);
        hEditKeywords=mkE(L"",10,Y5,0,72,(HMENU)ID_EDIT_KEYWORDS,true); // width in WM_SIZE

        // Row 6: 按钮
        hBtnSearch=mkB(L"开始搜索", 10,Y6,110,30,(HMENU)ID_BTN_SEARCH);
        hBtnClear =mkB(L"清空",    130,Y6, 80,30,(HMENU)ID_BTN_CLEAR);
        hBtnCopy  =mkB(L"复制结果",220,Y6,110,30,(HMENU)ID_BTN_COPY);
        EnableWindow(hBtnCopy,FALSE);
        hLabelStatus=CreateWindow(L"STATIC",L"就绪",WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE,
                     344,Y6,400,30,hw,(HMENU)ID_LABEL_STATUS,NULL,NULL);
        SendMessage(hLabelStatus,WM_SETFONT,(WPARAM)hFontUI,TRUE);

        // Row 7: 结果
        hEditResult=CreateWindow(L"EDIT",L"",
                    WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE|ES_READONLY|
                    WS_VSCROLL|WS_HSCROLL|ES_AUTOVSCROLL|ES_AUTOHSCROLL,
                    10,Y7,0,200,hw,(HMENU)ID_EDIT_RESULT,NULL,NULL);
        SendMessage(hEditResult,WM_SETFONT,(WPARAM)hFontMono,TRUE);

        // trigger initial layout
        RECT rc;GetClientRect(hw,&rc);
        SendMessage(hw,WM_SIZE,0,MAKELPARAM(rc.right,rc.bottom));
        break;}

    case WM_ERASEBKGND:{RECT rc;GetClientRect(hw,&rc);FillRect((HDC)wp,&rc,hBrushBg);return 1;}
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:{SetTextColor((HDC)wp,CLR_TEXT);SetBkColor((HDC)wp,CLR_PANEL);return(LRESULT)hBrushPanel;}
    case WM_DRAWITEM:{
        auto* di=(DRAWITEMSTRUCT*)lp;
        if     (di->CtlID==ID_BTN_SEARCH) DrawBtn(di,CLR_ACCENT,  CLR_ACCENT_H,   L"开始搜索");
        else if(di->CtlID==ID_BTN_CLEAR)  DrawBtn(di,CLR_BTN_CLR, RGB(80,88,96),  L"清空");
        else if(di->CtlID==ID_BTN_COPY)   DrawBtn(di,CLR_BTN_CPY, RGB(30,130,55), L"复制结果");
        else if(di->CtlID==ID_BTN_BROWSE) DrawBtn(di,RGB(73,80,87),RGB(52,58,64), L"浏览...");
        return TRUE;}
    case WM_COMMAND:
        switch(LOWORD(wp)){
        case ID_BTN_BROWSE:BrowseDir(hw);break;
        case ID_BTN_SEARCH:StartSearch(hw);break;
        case ID_BTN_COPY:  CopyResults();break;
        case ID_BTN_CLEAR:
            SetWindowText(hEditResult,L"");SetWindowText(hLabelStatus,L"就绪");
            EnableWindow(hBtnCopy,FALSE);break;
        }break;
    case WM_SIZE:{
        int w=LOWORD(lp),h=HIWORD(lp);
        if(w<200||h<200)break;
        int R=w-10; // right edge
        // Row0: [label=84] [EditDir .. R-70] [Browse R-64..R]
        MoveWindow(hEditDir,   84,  Y0, R-84-66, LH,TRUE);
        MoveWindow(hBtnBrowse, R-62,Y0, 60,      LH,TRUE);
        // Row1: fixed positions, no stretch needed
        MoveWindow(hLabelStatus,344,Y6,R-354,30,TRUE);
        // Row3: ext field full width
        MoveWindow(hEditExt,   10,  Y3, R-10, LH,TRUE);
        // Row5: keywords full width
        MoveWindow(hEditKeywords,10,Y5,R-10,72,TRUE);
        // Row7: result
        MoveWindow(hEditResult,10,  Y7, R-10, h-Y7-10,TRUE);
        break;}
    case WM_DESTROY:PostQuitMessage(0);break;
    default:return DefWindowProc(hw,msg,wp,lp);
    }return 0;
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int nc){
    CoInitialize(NULL);
    WNDCLASSEX wc={sizeof(wc)};
    wc.lpfnWndProc=WndProc;wc.hInstance=hi;
    wc.lpszClassName=L"TxtSrch5";
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
    RegisterClassEx(&wc);
    HWND hw=CreateWindowEx(0,L"TxtSrch5",L"文本文件关键词搜索器",
            WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,900,680,
            NULL,NULL,hi,NULL);
    ShowWindow(hw,nc);UpdateWindow(hw);
    MSG m;while(GetMessage(&m,NULL,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    CoUninitialize();return 0;
}
