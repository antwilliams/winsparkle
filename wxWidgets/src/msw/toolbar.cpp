/////////////////////////////////////////////////////////////////////////////
// Name:        src/msw/toolbar.cpp
// Purpose:     wxToolBar
// Author:      Julian Smart
// Modified by:
// Created:     04/01/98
// RCS-ID:      $Id: toolbar.cpp 62850 2009-12-10 03:04:19Z VZ $
// Copyright:   (c) Julian Smart
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

// ============================================================================
// declarations
// ============================================================================

// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

#ifdef __BORLANDC__
    #pragma hdrstop
#endif

#if wxUSE_TOOLBAR && wxUSE_TOOLBAR_NATIVE && !defined(__SMARTPHONE__)

#include "wx/toolbar.h"

#ifndef WX_PRECOMP
    #include "wx/msw/wrapcctl.h" // include <commctrl.h> "properly"
    #include "wx/dynarray.h"
    #include "wx/frame.h"
    #include "wx/log.h"
    #include "wx/intl.h"
    #include "wx/settings.h"
    #include "wx/bitmap.h"
    #include "wx/dcmemory.h"
    #include "wx/control.h"
    #include "wx/app.h"         // for GetComCtl32Version
    #include "wx/image.h"
    #include "wx/stattext.h"
#endif

#include "wx/artprov.h"
#include "wx/sysopt.h"
#include "wx/dcclient.h"
#include "wx/scopedarray.h"

#include "wx/msw/private.h"
#include "wx/msw/dc.h"

#if wxUSE_UXTHEME
#include "wx/msw/uxtheme.h"
#endif

// this define controls whether the code for button colours remapping (only
// useful for 16 or 256 colour images) is active at all, it's always turned off
// for CE where it doesn't compile (and is probably not needed anyhow) and may
// also be turned off for other systems if you always use 24bpp images and so
// never need it
#ifndef __WXWINCE__
    #define wxREMAP_BUTTON_COLOURS
#endif // !__WXWINCE__

// ----------------------------------------------------------------------------
// constants
// ----------------------------------------------------------------------------

// these standard constants are not always defined in compilers headers

// Styles
#ifndef TBSTYLE_FLAT
    #define TBSTYLE_LIST            0x1000
    #define TBSTYLE_FLAT            0x0800
#endif

#ifndef TBSTYLE_TRANSPARENT
    #define TBSTYLE_TRANSPARENT     0x8000
#endif

#ifndef TBSTYLE_TOOLTIPS
    #define TBSTYLE_TOOLTIPS        0x0100
#endif

// Messages
#ifndef TB_GETSTYLE
    #define TB_SETSTYLE             (WM_USER + 56)
    #define TB_GETSTYLE             (WM_USER + 57)
#endif

#ifndef TB_HITTEST
    #define TB_HITTEST              (WM_USER + 69)
#endif

#ifndef TB_GETMAXSIZE
    #define TB_GETMAXSIZE           (WM_USER + 83)
#endif

// ----------------------------------------------------------------------------
// wxWin macros
// ----------------------------------------------------------------------------

IMPLEMENT_DYNAMIC_CLASS(wxToolBar, wxControl)

/*
    TOOLBAR PROPERTIES
        tool
            bitmap
            bitmap2
            tooltip
            longhelp
            radio (bool)
            toggle (bool)
        separator
        style ( wxNO_BORDER | wxTB_HORIZONTAL)
        bitmapsize
        margins
        packing
        separation

        dontattachtoframe
*/

BEGIN_EVENT_TABLE(wxToolBar, wxToolBarBase)
    EVT_MOUSE_EVENTS(wxToolBar::OnMouseEvent)
    EVT_SYS_COLOUR_CHANGED(wxToolBar::OnSysColourChanged)
    EVT_ERASE_BACKGROUND(wxToolBar::OnEraseBackground)
END_EVENT_TABLE()

// ----------------------------------------------------------------------------
// private classes
// ----------------------------------------------------------------------------

class wxToolBarTool : public wxToolBarToolBase
{
public:
    wxToolBarTool(wxToolBar *tbar,
                  int id,
                  const wxString& label,
                  const wxBitmap& bmpNormal,
                  const wxBitmap& bmpDisabled,
                  wxItemKind kind,
                  wxObject *clientData,
                  const wxString& shortHelp,
                  const wxString& longHelp)
        : wxToolBarToolBase(tbar, id, label, bmpNormal, bmpDisabled, kind,
                            clientData, shortHelp, longHelp)
    {
        m_nSepCount = 0;
        m_staticText = NULL;
    }

    wxToolBarTool(wxToolBar *tbar, wxControl *control, const wxString& label)
        : wxToolBarToolBase(tbar, control, label)
    {
        if ( IsControl() && !m_label.empty() )
        {
            // create a control to render the control's label
            m_staticText = new wxStaticText
                               (
                                 m_tbar,
                                 wxID_ANY,
                                 m_label,
                                 wxDefaultPosition,
                                 wxDefaultSize,
                                 wxALIGN_CENTRE | wxST_NO_AUTORESIZE
                               );
        }
        else // no label
        {
            m_staticText = NULL;
        }

        m_nSepCount = 1;
    }

    virtual ~wxToolBarTool()
    {
        delete m_staticText;
    }

    virtual void SetLabel(const wxString& label)
    {
        if ( label == m_label )
            return;

        wxToolBarToolBase::SetLabel(label);

        if ( m_staticText )
            m_staticText->SetLabel(label);

        // we need to update the label shown in the toolbar because it has a
        // pointer to the internal buffer of the old label
        //
        // TODO: use TB_SETBUTTONINFO
    }

    wxStaticText* GetStaticText()
    {
        wxASSERT_MSG( IsControl(),
                      wxT("only makes sense for embedded control tools") );

        return m_staticText;
    }

    // set/get the number of separators which we use to cover the space used by
    // a control in the toolbar
    void SetSeparatorsCount(size_t count) { m_nSepCount = count; }
    size_t GetSeparatorsCount() const { return m_nSepCount; }

    // we need ids for the spacers which we want to modify later on, this
    // function will allocate a valid/unique id for a spacer if not done yet
    void AllocSpacerId()
    {
        if ( m_id == wxID_SEPARATOR )
            m_id = wxWindow::NewControlId();
    }

    // this method is used for controls only and offsets the control by the
    // given amount (in pixels) in horizontal direction
    void MoveBy(int offset)
    {
        wxControl * const control = GetControl();

        control->Move(control->GetPosition().x + offset, wxDefaultCoord);

        if ( m_staticText )
        {
            m_staticText->Move(m_staticText->GetPosition().x + offset,
                               wxDefaultCoord);
        }
    }

private:
    size_t m_nSepCount;
    wxStaticText *m_staticText;

    wxDECLARE_NO_COPY_CLASS(wxToolBarTool);
};

// ----------------------------------------------------------------------------
// helper functions
// ----------------------------------------------------------------------------

// return the rectangle of the item at the given index
//
// returns an empty (0, 0, 0, 0) rectangle if fails so the caller may compare
// r.right or r.bottom with 0 to check for this
static RECT wxGetTBItemRect(HWND hwnd, int index)
{
    RECT r;

    // note that we use TB_GETITEMRECT and not TB_GETRECT because the latter
    // only appeared in v4.70 of comctl32.dll
    if ( !::SendMessage(hwnd, TB_GETITEMRECT, index, (LPARAM)&r) )
    {
        wxLogLastError(wxT("TB_GETITEMRECT"));

        r.top =
        r.left =
        r.right =
        r.bottom = 0;
    }

    return r;
}

// ============================================================================
// implementation
// ============================================================================

// ----------------------------------------------------------------------------
// wxToolBarTool
// ----------------------------------------------------------------------------

wxToolBarToolBase *wxToolBar::CreateTool(int id,
                                         const wxString& label,
                                         const wxBitmap& bmpNormal,
                                         const wxBitmap& bmpDisabled,
                                         wxItemKind kind,
                                         wxObject *clientData,
                                         const wxString& shortHelp,
                                         const wxString& longHelp)
{
    return new wxToolBarTool(this, id, label, bmpNormal, bmpDisabled, kind,
                             clientData, shortHelp, longHelp);
}

wxToolBarToolBase *
wxToolBar::CreateTool(wxControl *control, const wxString& label)
{
    return new wxToolBarTool(this, control, label);
}

// ----------------------------------------------------------------------------
// wxToolBar construction
// ----------------------------------------------------------------------------

void wxToolBar::Init()
{
    m_hBitmap = 0;
    m_disabledImgList = NULL;

    m_nButtons = 0;
    m_totalFixedSize = 0;

    // even though modern Windows applications typically use 24*24 (or even
    // 32*32) size for their bitmaps, the native control itself still uses the
    // old 16*15 default size (see TB_SETBITMAPSIZE documentation in MSDN), so
    // default to it so that we don't call SetToolBitmapSize() unnecessarily in
    // wxToolBarBase::AdjustToolBitmapSize()
    m_defaultWidth = 16;
    m_defaultHeight = 15;

    m_pInTool = NULL;
}

bool wxToolBar::Create(wxWindow *parent,
                       wxWindowID id,
                       const wxPoint& pos,
                       const wxSize& size,
                       long style,
                       const wxString& name)
{
    // common initialisation
    if ( !CreateControl(parent, id, pos, size, style, wxDefaultValidator, name) )
        return false;

    FixupStyle();

    // MSW-specific initialisation
    if ( !MSWCreateToolbar(pos, size) )
        return false;

    wxSetCCUnicodeFormat(GetHwnd());

    // workaround for flat toolbar on Windows XP classic style: we have to set
    // the style after creating the control; doing it at creation time doesn't work
#if wxUSE_UXTHEME
    if ( style & wxTB_FLAT )
    {
        LRESULT style = GetMSWToolbarStyle();

        if ( !(style & TBSTYLE_FLAT) )
            ::SendMessage(GetHwnd(), TB_SETSTYLE, 0, style | TBSTYLE_FLAT);
    }
#endif // wxUSE_UXTHEME

    return true;
}

bool wxToolBar::MSWCreateToolbar(const wxPoint& pos, const wxSize& size)
{
    if ( !MSWCreateControl(TOOLBARCLASSNAME, wxEmptyString, pos, size) )
        return false;

    // toolbar-specific post initialisation
    ::SendMessage(GetHwnd(), TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

#ifdef TB_SETEXTENDEDSTYLE
    if ( wxApp::GetComCtl32Version() >= 471 )
        ::SendMessage(GetHwnd(), TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS);
#endif

    return true;
}

void wxToolBar::Recreate()
{
    const HWND hwndOld = GetHwnd();
    if ( !hwndOld )
    {
        // we haven't been created yet, no need to recreate
        return;
    }

    // get the position and size before unsubclassing the old toolbar
    const wxPoint pos = GetPosition();
    const wxSize size = GetSize();

    UnsubclassWin();

    if ( !MSWCreateToolbar(pos, size) )
    {
        // what can we do?
        wxFAIL_MSG( wxT("recreating the toolbar failed") );

        return;
    }

    // reparent all our children under the new toolbar
    for ( wxWindowList::compatibility_iterator node = m_children.GetFirst();
          node;
          node = node->GetNext() )
    {
        wxWindow *win = node->GetData();
        if ( !win->IsTopLevel() )
            ::SetParent(GetHwndOf(win), GetHwnd());
    }

    // only destroy the old toolbar now --
    // after all the children had been reparented
    ::DestroyWindow(hwndOld);

    // it is for the old bitmap control and can't be used with the new one
    if ( m_hBitmap )
    {
        ::DeleteObject((HBITMAP) m_hBitmap);
        m_hBitmap = 0;
    }

    if ( m_disabledImgList )
    {
        delete m_disabledImgList;
        m_disabledImgList = NULL;
    }

    Realize();
}

wxToolBar::~wxToolBar()
{
    // we must refresh the frame size when the toolbar is deleted but the frame
    // is not - otherwise toolbar leaves a hole in the place it used to occupy
    SendSizeEventToParent();

    if ( m_hBitmap )
        ::DeleteObject((HBITMAP) m_hBitmap);

    delete m_disabledImgList;
}

wxSize wxToolBar::DoGetBestSize() const
{
    wxSize sizeBest;

    SIZE size;
    if ( !::SendMessage(GetHwnd(), TB_GETMAXSIZE, 0, (LPARAM)&size) )
    {
        // maybe an old (< 0x400) Windows version? try to approximate the
        // toolbar size ourselves
        sizeBest = GetToolSize();
        sizeBest.y += 2 * ::GetSystemMetrics(SM_CYBORDER); // Add borders
        sizeBest.x *= GetToolsCount();

        // reverse horz and vertical components if necessary
        if ( IsVertical() )
        {
            int t = sizeBest.x;
            sizeBest.x = sizeBest.y;
            sizeBest.y = t;
        }
    }
    else // TB_GETMAXSIZE succeeded
    {
        // but it could still return an incorrect result due to what appears to
        // be a bug in old comctl32.dll versions which don't handle controls in
        // the toolbar correctly, so work around it (see SF patch 1902358)
        if ( !IsVertical() && wxApp::GetComCtl32Version() < 600 )
        {
            // calculate the toolbar width in alternative way
            const RECT rcFirst = wxGetTBItemRect(GetHwnd(), 0);
            const RECT rcLast = wxGetTBItemRect(GetHwnd(), GetToolsCount() - 1);

            const int widthAlt = rcLast.right - rcFirst.left;
            if ( widthAlt > size.cx )
                size.cx = widthAlt;
        }

        sizeBest.x = size.cx;
        sizeBest.y = size.cy;
    }

    if ( !IsVertical() )
    {
        // Without the extra height, DoGetBestSize can report a size that's
        // smaller than the actual window, causing windows to overlap slightly
        // in some circumstances, leading to missing borders (especially noticeable
        // in AUI layouts).
        if (!(GetWindowStyle() & wxTB_NODIVIDER))
            sizeBest.y += 2;
        sizeBest.y ++;
    }

    CacheBestSize(sizeBest);

    return sizeBest;
}

WXDWORD wxToolBar::MSWGetStyle(long style, WXDWORD *exstyle) const
{
    // toolbars never have border, giving one to them results in broken
    // appearance
    WXDWORD msStyle = wxControl::MSWGetStyle
                      (
                        (style & ~wxBORDER_MASK) | wxBORDER_NONE, exstyle
                      );

    if ( !(style & wxTB_NO_TOOLTIPS) )
        msStyle |= TBSTYLE_TOOLTIPS;

    if ( style & (wxTB_FLAT | wxTB_HORZ_LAYOUT) )
    {
        // static as it doesn't change during the program lifetime
        static const int s_verComCtl = wxApp::GetComCtl32Version();

        // comctl32.dll 4.00 doesn't support the flat toolbars and using this
        // style with 6.00 (part of Windows XP) leads to the toolbar with
        // incorrect background colour - and not using it still results in the
        // correct (flat) toolbar, so don't use it there
        if ( s_verComCtl > 400 && s_verComCtl < 600 )
            msStyle |= TBSTYLE_FLAT | TBSTYLE_TRANSPARENT;

        if ( s_verComCtl >= 470 && style & wxTB_HORZ_LAYOUT )
            msStyle |= TBSTYLE_LIST;
    }

    if ( style & wxTB_NODIVIDER )
        msStyle |= CCS_NODIVIDER;

    if ( style & wxTB_NOALIGN )
        msStyle |= CCS_NOPARENTALIGN;

    if ( style & wxTB_VERTICAL )
        msStyle |= CCS_VERT;

    if( style & wxTB_BOTTOM )
        msStyle |= CCS_BOTTOM;

    if ( style & wxTB_RIGHT )
        msStyle |= CCS_RIGHT;

    return msStyle;
}

// ----------------------------------------------------------------------------
// adding/removing tools
// ----------------------------------------------------------------------------

bool wxToolBar::DoInsertTool(size_t WXUNUSED(pos),
                             wxToolBarToolBase * WXUNUSED(tool))
{
    // nothing special to do here - we really create the toolbar buttons in
    // Realize() later
    InvalidateBestSize();
    return true;
}

bool wxToolBar::DoDeleteTool(size_t pos, wxToolBarToolBase *tool)
{
    // the main difficulty we have here is with the controls in the toolbars:
    // as we (sometimes) use several separators to cover up the space used by
    // them, the indices are not the same for us and the toolbar

    // first determine the position of the first button to delete: it may be
    // different from pos if we use several separators to cover the space used
    // by a control
    wxToolBarToolsList::compatibility_iterator node;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarToolBase *tool2 = node->GetData();
        if ( tool2 == tool )
        {
            // let node point to the next node in the list
            node = node->GetNext();

            break;
        }

        if ( tool2->IsControl() )
            pos += ((wxToolBarTool *)tool2)->GetSeparatorsCount() - 1;
    }

    // now determine the number of buttons to delete and the area taken by them
    size_t nButtonsToDelete = 1;

    // get the size of the button we're going to delete
    const RECT r = wxGetTBItemRect(GetHwnd(), pos);

    int width = r.right - r.left;

    if ( tool->IsControl() )
    {
        nButtonsToDelete = ((wxToolBarTool *)tool)->GetSeparatorsCount();
        width *= nButtonsToDelete;
    }

    // do delete all buttons
    m_nButtons -= nButtonsToDelete;
    while ( nButtonsToDelete-- > 0 )
    {
        if ( !::SendMessage(GetHwnd(), TB_DELETEBUTTON, pos, 0) )
        {
            wxLogLastError(wxT("TB_DELETEBUTTON"));

            return false;
        }
    }

    // and finally reposition all the controls after this button (the toolbar
    // takes care of all normal items)
    for ( /* node -> first after deleted */ ; node; node = node->GetNext() )
    {
        wxToolBarTool *tool2 = (wxToolBarTool*)node->GetData();
        if ( tool2->IsControl() )
        {
            tool2->MoveBy(-width);
        }
    }

    InvalidateBestSize();

    return true;
}

void wxToolBar::CreateDisabledImageList()
{
    if (m_disabledImgList != NULL)
    {
        delete m_disabledImgList;
        m_disabledImgList = NULL;
    }

    // as we can't use disabled image list with older versions of comctl32.dll,
    // don't even bother creating it
    if ( wxApp::GetComCtl32Version() >= 470 )
    {
        // search for the first disabled button img in the toolbar, if any
        for ( wxToolBarToolsList::compatibility_iterator
                node = m_tools.GetFirst(); node; node = node->GetNext() )
        {
            wxToolBarToolBase *tool = node->GetData();
            wxBitmap bmpDisabled = tool->GetDisabledBitmap();
            if ( bmpDisabled.Ok() )
            {
                m_disabledImgList = new wxImageList
                                        (
                                            m_defaultWidth,
                                            m_defaultHeight,
                                            bmpDisabled.GetMask() != NULL,
                                            GetToolsCount()
                                        );
                break;
            }
        }

        // we don't have any disabled bitmaps
    }
}

bool wxToolBar::Realize()
{
    if ( !wxToolBarBase::Realize() )
        return false;

    const size_t nTools = GetToolsCount();

#ifdef wxREMAP_BUTTON_COLOURS
    // don't change the values of these constants, they can be set from the
    // user code via wxSystemOptions
    enum
    {
        Remap_None = -1,
        Remap_Bg,
        Remap_Buttons,
        Remap_TransparentBg
    };

    // the user-specified option overrides anything, but if it wasn't set, only
    // remap the buttons on 8bpp displays as otherwise the bitmaps usually look
    // much worse after remapping
    static const wxChar *remapOption = wxT("msw.remap");
    const int remapValue = wxSystemOptions::HasOption(remapOption)
                                ? wxSystemOptions::GetOptionInt(remapOption)
                                : wxDisplayDepth() <= 8 ? Remap_Buttons
                                                        : Remap_None;

#endif // wxREMAP_BUTTON_COLOURS

    // delete all old buttons, if any
    for ( size_t pos = 0; pos < m_nButtons; pos++ )
    {
        if ( !::SendMessage(GetHwnd(), TB_DELETEBUTTON, 0, 0) )
        {
            wxLogDebug(wxT("TB_DELETEBUTTON failed"));
        }
    }

    // First, add the bitmap: we use one bitmap for all toolbar buttons
    // ----------------------------------------------------------------

    wxToolBarToolsList::compatibility_iterator node;
    int bitmapId = 0;

    if ( !HasFlag(wxTB_NOICONS) )
    {
        // if we already have a bitmap, we'll replace the existing one --
        // otherwise we'll install a new one
        HBITMAP oldToolBarBitmap = (HBITMAP)m_hBitmap;

        const wxCoord totalBitmapWidth  = m_defaultWidth *
                                          wx_truncate_cast(wxCoord, nTools),
                      totalBitmapHeight = m_defaultHeight;

        // Create a bitmap and copy all the tool bitmaps into it
        wxMemoryDC dcAllButtons;
        wxBitmap bitmap(totalBitmapWidth, totalBitmapHeight);
        dcAllButtons.SelectObject(bitmap);

#ifdef wxREMAP_BUTTON_COLOURS
        if ( remapValue != Remap_TransparentBg )
#endif // wxREMAP_BUTTON_COLOURS
        {
            // VZ: why do we hardcode grey colour for CE?
            dcAllButtons.SetBackground(wxBrush(
#ifdef __WXWINCE__
                                        wxColour(0xc0, 0xc0, 0xc0)
#else // !__WXWINCE__
                                        GetBackgroundColour()
#endif // __WXWINCE__/!__WXWINCE__
                                       ));
            dcAllButtons.Clear();
        }

        m_hBitmap = bitmap.GetHBITMAP();
        HBITMAP hBitmap = (HBITMAP)m_hBitmap;

#ifdef wxREMAP_BUTTON_COLOURS
        if ( remapValue == Remap_Bg )
        {
            dcAllButtons.SelectObject(wxNullBitmap);

            // Even if we're not remapping the bitmap
            // content, we still have to remap the background.
            hBitmap = (HBITMAP)MapBitmap((WXHBITMAP) hBitmap,
                totalBitmapWidth, totalBitmapHeight);

            dcAllButtons.SelectObject(bitmap);
        }
#endif // wxREMAP_BUTTON_COLOURS

        // the button position
        wxCoord x = 0;

        // the number of buttons (not separators)
        int nButtons = 0;

        CreateDisabledImageList();
        for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
        {
            wxToolBarToolBase *tool = node->GetData();
            if ( tool->IsButton() )
            {
                const wxBitmap& bmp = tool->GetNormalBitmap();

                const int w = bmp.GetWidth();
                const int h = bmp.GetHeight();

                if ( bmp.Ok() )
                {
                    int xOffset = wxMax(0, (m_defaultWidth - w)/2);
                    int yOffset = wxMax(0, (m_defaultHeight - h)/2);

                    // notice the last parameter: do use mask
                    dcAllButtons.DrawBitmap(bmp, x + xOffset, yOffset, true);
                }
                else
                {
                    wxFAIL_MSG( wxT("invalid tool button bitmap") );
                }

                // also deal with disabled bitmap if we want to use them
                if ( m_disabledImgList )
                {
                    wxBitmap bmpDisabled = tool->GetDisabledBitmap();
#if wxUSE_IMAGE && wxUSE_WXDIB
                    if ( !bmpDisabled.Ok() )
                    {
                        // no disabled bitmap specified but we still need to
                        // fill the space in the image list with something, so
                        // we grey out the normal bitmap
                        wxImage
                          imgGreyed = bmp.ConvertToImage().ConvertToGreyscale();

#ifdef wxREMAP_BUTTON_COLOURS
                        if ( remapValue == Remap_Buttons )
                        {
                            // we need to have light grey background colour for
                            // MapBitmap() to work correctly
                            for ( int y = 0; y < h; y++ )
                            {
                                for ( int x = 0; x < w; x++ )
                                {
                                    if ( imgGreyed.IsTransparent(x, y) )
                                        imgGreyed.SetRGB(x, y,
                                                         wxLIGHT_GREY->Red(),
                                                         wxLIGHT_GREY->Green(),
                                                         wxLIGHT_GREY->Blue());
                                }
                            }
                        }
#endif // wxREMAP_BUTTON_COLOURS

                        bmpDisabled = wxBitmap(imgGreyed);
                    }
#endif // wxUSE_IMAGE

#ifdef wxREMAP_BUTTON_COLOURS
                    if ( remapValue == Remap_Buttons )
                        MapBitmap(bmpDisabled.GetHBITMAP(), w, h);
#endif // wxREMAP_BUTTON_COLOURS

                    m_disabledImgList->Add(bmpDisabled);
                }

                // still inc width and number of buttons because otherwise the
                // subsequent buttons will all be shifted which is rather confusing
                // (and like this you'd see immediately which bitmap was bad)
                x += m_defaultWidth;
                nButtons++;
            }
        }

        dcAllButtons.SelectObject(wxNullBitmap);

        // don't delete this HBITMAP!
        bitmap.SetHBITMAP(0);

#ifdef wxREMAP_BUTTON_COLOURS
        if ( remapValue == Remap_Buttons )
        {
            // Map to system colours
            hBitmap = (HBITMAP)MapBitmap((WXHBITMAP) hBitmap,
                                         totalBitmapWidth, totalBitmapHeight);
        }
#endif // wxREMAP_BUTTON_COLOURS

        bool addBitmap = true;

        if ( oldToolBarBitmap )
        {
#ifdef TB_REPLACEBITMAP
            if ( wxApp::GetComCtl32Version() >= 400 )
            {
                TBREPLACEBITMAP replaceBitmap;
                replaceBitmap.hInstOld = NULL;
                replaceBitmap.hInstNew = NULL;
                replaceBitmap.nIDOld = (UINT_PTR)oldToolBarBitmap;
                replaceBitmap.nIDNew = (UINT_PTR)hBitmap;
                replaceBitmap.nButtons = nButtons;
                if ( !::SendMessage(GetHwnd(), TB_REPLACEBITMAP,
                                    0, (LPARAM) &replaceBitmap) )
                {
                    wxFAIL_MSG(wxT("Could not replace the old bitmap"));
                }

                ::DeleteObject(oldToolBarBitmap);

                // already done
                addBitmap = false;
            }
            else
#endif // TB_REPLACEBITMAP
            {
                // we can't replace the old bitmap, so we will add another one
                // (awfully inefficient, but what else to do?) and shift the bitmap
                // indices accordingly
                addBitmap = true;

                bitmapId = m_nButtons;
            }
        }

        if ( addBitmap ) // no old bitmap or we can't replace it
        {
            TBADDBITMAP addBitmap;
            addBitmap.hInst = 0;
            addBitmap.nID = (UINT_PTR)hBitmap;
            if ( ::SendMessage(GetHwnd(), TB_ADDBITMAP,
                               (WPARAM) nButtons, (LPARAM)&addBitmap) == -1 )
            {
                wxFAIL_MSG(wxT("Could not add bitmap to toolbar"));
            }
        }

        // disable image lists are only supported in comctl32.dll 4.70+
        if ( wxApp::GetComCtl32Version() >= 470 )
        {
            HIMAGELIST hil = m_disabledImgList
                                ? GetHimagelistOf(m_disabledImgList)
                                : 0;

            // notice that we set the image list even if don't have one right
            // now as we could have it before and need to reset it in this case
            HIMAGELIST oldImageList = (HIMAGELIST)
              ::SendMessage(GetHwnd(), TB_SETDISABLEDIMAGELIST, 0, (LPARAM)hil);

            // delete previous image list if any
            if ( oldImageList )
                ::DeleteObject(oldImageList);
        }
    }


    // Next add the buttons and separators
    // -----------------------------------

    wxScopedArray<TBBUTTON> buttons(new TBBUTTON[nTools]);

    // this array will hold the indices of all controls in the toolbar
    wxArrayInt controlIds;

    bool lastWasRadio = false;
    int i = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarTool *tool = static_cast<wxToolBarTool *>(node->GetData());

        // don't add separators to the vertical toolbar with old comctl32.dll
        // versions as they didn't handle this properly
        if ( IsVertical() && tool->IsSeparator() &&
                wxApp::GetComCtl32Version() <= 472 )
        {
            continue;
        }

        TBBUTTON& button = buttons[i];

        wxZeroMemory(button);

        bool isRadio = false;
        switch ( tool->GetStyle() )
        {
            case wxTOOL_STYLE_CONTROL:
            case wxTOOL_STYLE_SEPARATOR:
                if ( tool->IsStretchableSpace() )
                {
                    // we're going to modify the size of this button later and
                    // so we need a valid id for it and not wxID_SEPARATOR
                    // which is used by spacers by default
                    tool->AllocSpacerId();
                }

                button.idCommand = tool->GetId();
                button.fsState = TBSTATE_ENABLED;
                button.fsStyle = TBSTYLE_SEP;
                break;

            case wxTOOL_STYLE_BUTTON:
                if ( !HasFlag(wxTB_NOICONS) )
                    button.iBitmap = bitmapId;

                if ( HasFlag(wxTB_TEXT) )
                {
                    const wxString& label = tool->GetLabel();
                    if ( !label.empty() )
                        button.iString = (INT_PTR)label.wx_str();
                }

                button.idCommand = tool->GetId();

                if ( tool->IsEnabled() )
                    button.fsState |= TBSTATE_ENABLED;
                if ( tool->IsToggled() )
                    button.fsState |= TBSTATE_CHECKED;

                switch ( tool->GetKind() )
                {
                    case wxITEM_RADIO:
                        button.fsStyle = TBSTYLE_CHECKGROUP;

                        if ( !lastWasRadio )
                        {
                            // the first item in the radio group is checked by
                            // default to be consistent with wxGTK and the menu
                            // radio items
                            button.fsState |= TBSTATE_CHECKED;

                            if (tool->Toggle(true))
                            {
                                DoToggleTool(tool, true);
                            }
                        }
                        else if ( tool->IsToggled() )
                        {
                            wxToolBarToolsList::compatibility_iterator nodePrev = node->GetPrevious();
                            int prevIndex = i - 1;
                            while ( nodePrev )
                            {
                                TBBUTTON& prevButton = buttons[prevIndex];
                                wxToolBarToolBase *tool = nodePrev->GetData();
                                if ( !tool->IsButton() || tool->GetKind() != wxITEM_RADIO )
                                    break;

                                if ( tool->Toggle(false) )
                                    DoToggleTool(tool, false);

                                prevButton.fsState &= ~TBSTATE_CHECKED;
                                nodePrev = nodePrev->GetPrevious();
                                prevIndex--;
                            }
                        }

                        isRadio = true;
                        break;

                    case wxITEM_CHECK:
                        button.fsStyle = TBSTYLE_CHECK;
                        break;

                    case wxITEM_NORMAL:
                        button.fsStyle = TBSTYLE_BUTTON;
                        break;

                   case wxITEM_DROPDOWN:
                        button.fsStyle = TBSTYLE_DROPDOWN;
                        break;

                    default:
                        wxFAIL_MSG( wxT("unexpected toolbar button kind") );
                        button.fsStyle = TBSTYLE_BUTTON;
                        break;
                }

                bitmapId++;
                break;
        }

        lastWasRadio = isRadio;

        i++;
    }

    if ( !::SendMessage(GetHwnd(), TB_ADDBUTTONS, i, (LPARAM)buttons.get()) )
    {
        wxLogLastError(wxT("TB_ADDBUTTONS"));
    }


    // Adjust controls and stretchable spaces
    // --------------------------------------

    // adjust the controls size to fit nicely in the toolbar and compute its
    // total size while doing it
    m_totalFixedSize = 0;
    int toolIndex = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext(), toolIndex++ )
    {
        wxToolBarTool * const tool = (wxToolBarTool*)node->GetData();

        const RECT r = wxGetTBItemRect(GetHwnd(), toolIndex);

        if ( !tool->IsControl() )
        {
            if ( IsVertical() )
                m_totalFixedSize += r.bottom - r.top;
            else
                m_totalFixedSize += r.right - r.left;

            continue;
        }

        if ( IsVertical() )
        {
            // don't embed controls in the vertical toolbar, this doesn't look
            // good and wxGTK doesn't do it neither (and the code below can't
            // deal with this case)
            continue;
        }

        wxControl * const control = tool->GetControl();
        wxStaticText * const staticText = tool->GetStaticText();

        wxSize size = control->GetSize();
        wxSize staticTextSize;
        if ( staticText )
        {
            staticTextSize = staticText->GetSize();
            staticTextSize.y += 3; // margin between control and its label
        }

        // TB_SETBUTTONINFO message is only supported by comctl32.dll 4.71+
#ifdef TB_SETBUTTONINFO
        // available in headers, now check whether it is available now
        // (during run-time)
        if ( wxApp::GetComCtl32Version() >= 471 )
        {
            // set the (underlying) separators width to be that of the
            // control
            TBBUTTONINFO tbbi;
            tbbi.cbSize = sizeof(tbbi);
            tbbi.dwMask = TBIF_SIZE;
            tbbi.cx = (WORD)size.x;
            if ( !::SendMessage(GetHwnd(), TB_SETBUTTONINFO,
                                tool->GetId(), (LPARAM)&tbbi) )
            {
                // the id is probably invalid?
                wxLogLastError(wxT("TB_SETBUTTONINFO"));
            }
        }
        else
#endif // comctl32.dll 4.71
        // TB_SETBUTTONINFO unavailable
        {
            // try adding several separators to fit the controls width
            int widthSep = r.right - r.left;

            TBBUTTON tbb;
            wxZeroMemory(tbb);
            tbb.idCommand = 0;
            tbb.fsState = TBSTATE_ENABLED;
            tbb.fsStyle = TBSTYLE_SEP;

            size_t nSeparators = size.x / widthSep;
            for ( size_t nSep = 0; nSep < nSeparators; nSep++ )
            {
                if ( !::SendMessage(GetHwnd(), TB_INSERTBUTTON,
                                    toolIndex, (LPARAM)&tbb) )
                {
                    wxLogLastError(wxT("TB_INSERTBUTTON"));
                }

                toolIndex++;
            }

            // remember the number of separators we used - we'd have to
            // delete all of them later
            tool->SetSeparatorsCount(nSeparators);

            // adjust the controls width to exactly cover the separators
            size.x = (nSeparators + 1)*widthSep;
            control->SetSize(size.x, wxDefaultCoord);
        }

        // position the control itself correctly vertically centering it on the
        // icon area of the toolbar
        int height = r.bottom - r.top - staticTextSize.y;

        int diff = height - size.y;
        if ( diff < 0 || !HasFlag(wxTB_TEXT) )
        {
            // not enough room for the static text
            if ( staticText )
                staticText->Hide();

            // recalculate height & diff without the staticText control
            height = r.bottom - r.top;
            diff = height - size.y;
            if ( diff < 0 )
            {
                // the control is too high, resize to fit
                control->SetSize(wxDefaultCoord, height - 2);

                diff = 2;
            }
        }
        else // enough space for both the control and the label
        {
            if ( staticText )
                staticText->Show();
        }

        control->Move(r.left, r.top + (diff + 1) / 2);
        if ( staticText )
        {
            staticText->Move(r.left + (size.x - staticTextSize.x)/2,
                             r.bottom - staticTextSize.y);
        }

        m_totalFixedSize += size.x;
    }

    // the max index is the "real" number of buttons - i.e. counting even the
    // separators which we added just for aligning the controls
    m_nButtons = toolIndex;

    if ( !IsVertical() )
    {
        if ( m_maxRows == 0 )
            // if not set yet, only one row
            SetRows(1);
    }
    else if ( m_nButtons > 0 ) // vertical non empty toolbar
    {
        // if not set yet, have one column
        m_maxRows = 1;
        SetRows(m_nButtons);
    }

    InvalidateBestSize();
    UpdateSize();

    return true;
}

void wxToolBar::UpdateStretchableSpacersSize()
{
    // we can't resize the spacers if TB_SETBUTTONINFO is not supported
    if ( wxApp::GetComCtl32Version() < 471 )
        return;

    // check if we have any stretchable spacers in the first place
    unsigned numSpaces = 0;
    wxToolBarToolsList::compatibility_iterator node;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarTool * const tool = (wxToolBarTool*)node->GetData();
        if ( tool->IsStretchableSpace() )
            numSpaces++;
    }

    if ( !numSpaces )
        return;

    // we do, adjust their size: either distribute the extra size among them or
    // reduce their size if there is not enough place for all tools
    const int totalSize = IsVertical() ? GetClientSize().y : GetClientSize().x;
    const int extraSize = totalSize - m_totalFixedSize;
    const int sizeSpacer = extraSize > 0 ? extraSize / numSpaces : 0;

    // the last spacer should consume all remaining space if we have too much
    // of it (which can be greater than sizeSpacer because of the rounding)
    const int sizeLastSpacer = extraSize > 0
                                ? extraSize - (numSpaces - 1)*sizeSpacer
                                : 0;

    // cumulated offset by which we need to move all the following controls to
    // the right: while the toolbar takes care of the normal items, we must
    // move the controls manually ourselves to ensure they remain at the
    // correct place
    int offset = 0;
    int toolIndex = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext(), toolIndex++ )
    {
        wxToolBarTool * const tool = (wxToolBarTool*)node->GetData();

        if ( tool->IsControl() && offset )
        {
            tool->MoveBy(offset);

            continue;
        }

        if ( !tool->IsStretchableSpace() )
            continue;

        const RECT rcOld = wxGetTBItemRect(GetHwnd(), toolIndex);

        WinStruct<TBBUTTONINFO> tbbi;
        tbbi.dwMask = TBIF_SIZE;
        tbbi.cx = --numSpaces ? sizeSpacer : sizeLastSpacer;

        if ( !::SendMessage(GetHwnd(), TB_SETBUTTONINFO,
                            tool->GetId(), (LPARAM)&tbbi) )
        {
            wxLogLastError(wxT("TB_SETBUTTONINFO"));
        }
        else
        {
            // we successfully resized this one, move all the controls after it
            // by the corresponding amount (may be positive or negative)
            offset += tbbi.cx - (rcOld.right - rcOld.left);
        }
    }
}

// ----------------------------------------------------------------------------
// message handlers
// ----------------------------------------------------------------------------

bool wxToolBar::MSWCommand(WXUINT WXUNUSED(cmd), WXWORD id_)
{
    // cast to signed is important as we compare this id with (signed) ints in
    // FindById() and without the cast we'd get a positive int from a
    // "negative" (i.e. > 32767) WORD
    const int id = (signed short)id_;

    wxToolBarToolBase *tool = FindById(id);
    if ( !tool )
        return false;

    bool toggled = false; // just to suppress warnings

    LRESULT state = ::SendMessage(GetHwnd(), TB_GETSTATE, id, 0);

    if ( tool->CanBeToggled() )
    {
        toggled = (state & TBSTATE_CHECKED) != 0;

        // ignore the event when a radio button is released, as this doesn't
        // seem to happen at all, and is handled otherwise
        if ( tool->GetKind() == wxITEM_RADIO && !toggled )
            return true;

        tool->Toggle(toggled);
        UnToggleRadioGroup(tool);
    }

    // Without the two lines of code below, if the toolbar was repainted during
    // OnLeftClick(), then it could end up without the tool bitmap temporarily
    // (see http://lists.nongnu.org/archive/html/lmi/2008-10/msg00014.html).
    // The Update() call bellow ensures that this won't happen, by repainting
    // invalidated areas of the toolbar immediately.
    //
    // To complicate matters, the tool would be drawn in depressed state (this
    // code is called when mouse button is released, not pressed). That's not
    // ideal, having the tool pressed for the duration of OnLeftClick()
    // provides the user with useful visual clue that the app is busy reacting
    // to the event. So we manually put the tool into pressed state, handle the
    // event and then finally restore tool's original state.
    ::SendMessage(GetHwnd(), TB_SETSTATE, id, MAKELONG(state | TBSTATE_PRESSED, 0));
    Update();

    bool allowLeftClick = OnLeftClick(id, toggled);

    // Restore the unpressed state. Enabled/toggled state might have been
    // changed since so take care of it.
    if (tool->IsEnabled())
        state |= TBSTATE_ENABLED;
    else
        state &= ~TBSTATE_ENABLED;
    if (tool->IsToggled())
        state |= TBSTATE_CHECKED;
    else
        state &= ~TBSTATE_CHECKED;
    ::SendMessage(GetHwnd(), TB_SETSTATE, id, MAKELONG(state, 0));

    // OnLeftClick() can veto the button state change - for buttons which
    // may be toggled only, of couse
    if ( !allowLeftClick && tool->CanBeToggled() )
    {
        // revert back
        tool->Toggle(!toggled);

        ::SendMessage(GetHwnd(), TB_CHECKBUTTON, id, MAKELONG(!toggled, 0));
    }

    return true;
}

bool wxToolBar::MSWOnNotify(int WXUNUSED(idCtrl),
                            WXLPARAM lParam,
                            WXLPARAM *WXUNUSED(result))
{
    LPNMHDR hdr = (LPNMHDR)lParam;
    if ( hdr->code == TBN_DROPDOWN )
    {
        LPNMTOOLBAR tbhdr = (LPNMTOOLBAR)lParam;

        wxCommandEvent evt(wxEVT_COMMAND_TOOL_DROPDOWN_CLICKED, tbhdr->iItem);
        if ( HandleWindowEvent(evt) )
        {
            // Event got handled, don't display default popup menu
            return false;
        }

        const wxToolBarToolBase * const tool = FindById(tbhdr->iItem);
        wxCHECK_MSG( tool, false, wxT("drop down message for unknown tool") );

        wxMenu * const menu = tool->GetDropdownMenu();
        if ( !menu )
            return false;

        // Display popup menu below button
        const RECT r = wxGetTBItemRect(GetHwnd(), GetToolPos(tbhdr->iItem));
        if ( r.right )
            PopupMenu(menu, r.left, r.bottom);

        return true;
    }


    if( !HasFlag(wxTB_NO_TOOLTIPS) )
    {
#if wxUSE_TOOLTIPS
        // First check if this applies to us

        // the tooltips control created by the toolbar is sometimes Unicode, even
        // in an ANSI application - this seems to be a bug in comctl32.dll v5
        UINT code = hdr->code;
        if ( (code != (UINT) TTN_NEEDTEXTA) && (code != (UINT) TTN_NEEDTEXTW) )
            return false;

        HWND toolTipWnd = (HWND)::SendMessage(GetHwnd(), TB_GETTOOLTIPS, 0, 0);
        if ( toolTipWnd != hdr->hwndFrom )
            return false;

        LPTOOLTIPTEXT ttText = (LPTOOLTIPTEXT)lParam;
        int id = (int)ttText->hdr.idFrom;

        wxToolBarToolBase *tool = FindById(id);
        if ( tool )
            return HandleTooltipNotify(code, lParam, tool->GetShortHelp());
#else
        wxUnusedVar(lParam);
#endif
    }

    return false;
}

// ----------------------------------------------------------------------------
// toolbar geometry
// ----------------------------------------------------------------------------

void wxToolBar::SetToolBitmapSize(const wxSize& size)
{
    wxToolBarBase::SetToolBitmapSize(size);

    ::SendMessage(GetHwnd(), TB_SETBITMAPSIZE, 0, MAKELONG(size.x, size.y));
}

void wxToolBar::SetRows(int nRows)
{
    if ( nRows == m_maxRows )
    {
        // avoid resizing the frame uselessly
        return;
    }

    // TRUE in wParam means to create at least as many rows, FALSE -
    // at most as many
    RECT rect;
    ::SendMessage(GetHwnd(), TB_SETROWS,
                  MAKEWPARAM(nRows, !(GetWindowStyle() & wxTB_VERTICAL)),
                  (LPARAM) &rect);

    m_maxRows = nRows;

    UpdateSize();
}

// The button size is bigger than the bitmap size
wxSize wxToolBar::GetToolSize() const
{
    // TB_GETBUTTONSIZE is supported from version 4.70
#if defined(_WIN32_IE) && (_WIN32_IE >= 0x300 ) \
    && !( defined(__GNUWIN32__) && !wxCHECK_W32API_VERSION( 1, 0 ) ) \
    && !defined (__DIGITALMARS__)
    if ( wxApp::GetComCtl32Version() >= 470 )
    {
        DWORD dw = ::SendMessage(GetHwnd(), TB_GETBUTTONSIZE, 0, 0);

        return wxSize(LOWORD(dw), HIWORD(dw));
    }
    else
#endif // comctl32.dll 4.70+
    {
        // defaults
        return wxSize(m_defaultWidth + 8, m_defaultHeight + 7);
    }
}

static
wxToolBarToolBase *GetItemSkippingDummySpacers(const wxToolBarToolsList& tools,
                                               size_t index )
{
    wxToolBarToolsList::compatibility_iterator current = tools.GetFirst();

    for ( ; current ; current = current->GetNext() )
    {
        if ( index == 0 )
            return current->GetData();

        wxToolBarTool *tool = (wxToolBarTool *)current->GetData();
        size_t separators = tool->GetSeparatorsCount();

        // if it is a normal button, sepcount == 0, so skip 1 item (the button)
        // otherwise, skip as many items as the separator count, plus the
        // control itself
        index -= separators ? separators + 1 : 1;
    }

    return 0;
}

wxToolBarToolBase *wxToolBar::FindToolForPosition(wxCoord x, wxCoord y) const
{
    POINT pt;
    pt.x = x;
    pt.y = y;
    int index = (int)::SendMessage(GetHwnd(), TB_HITTEST, 0, (LPARAM)&pt);

    // MBN: when the point ( x, y ) is close to the toolbar border
    //      TB_HITTEST returns m_nButtons ( not -1 )
    if ( index < 0 || (size_t)index >= m_nButtons )
        // it's a separator or there is no tool at all there
        return NULL;

    // when TB_SETBUTTONINFO is available (both during compile- and run-time),
    // we don't use the dummy separators hack
#ifdef TB_SETBUTTONINFO
    if ( wxApp::GetComCtl32Version() >= 471 )
    {
        return m_tools.Item((size_t)index)->GetData();
    }
    else
#endif // TB_SETBUTTONINFO
    {
        return GetItemSkippingDummySpacers( m_tools, (size_t) index );
    }
}

void wxToolBar::UpdateSize()
{
    wxPoint pos = GetPosition();
    ::SendMessage(GetHwnd(), TB_AUTOSIZE, 0, 0);
    if (pos != GetPosition())
        Move(pos);

    // In case Realize is called after the initial display (IOW the programmer
    // may have rebuilt the toolbar) give the frame the option of resizing the
    // toolbar to full width again, but only if the parent is a frame and the
    // toolbar is managed by the frame.  Otherwise assume that some other
    // layout mechanism is controlling the toolbar size and leave it alone.
    SendSizeEventToParent();
}

// ----------------------------------------------------------------------------
// toolbar styles
// ---------------------------------------------------------------------------

// get the TBSTYLE of the given toolbar window
long wxToolBar::GetMSWToolbarStyle() const
{
    return ::SendMessage(GetHwnd(), TB_GETSTYLE, 0, 0L);
}

void wxToolBar::SetWindowStyleFlag(long style)
{
    // the style bits whose changes force us to recreate the toolbar
    static const long MASK_NEEDS_RECREATE = wxTB_TEXT | wxTB_NOICONS;

    const long styleOld = GetWindowStyle();

    wxToolBarBase::SetWindowStyleFlag(style);

    // don't recreate an empty toolbar: not only this is unnecessary, but it is
    // also fatal as we'd then try to recreate the toolbar when it's just being
    // created
    if ( GetToolsCount() &&
            (style & MASK_NEEDS_RECREATE) != (styleOld & MASK_NEEDS_RECREATE) )
    {
        // to remove the text labels, simply re-realizing the toolbar is enough
        // but I don't know of any way to add the text to an existing toolbar
        // other than by recreating it entirely
        Recreate();
    }
}

// ----------------------------------------------------------------------------
// tool state
// ----------------------------------------------------------------------------

void wxToolBar::DoEnableTool(wxToolBarToolBase *tool, bool enable)
{
    ::SendMessage(GetHwnd(), TB_ENABLEBUTTON,
                  (WPARAM)tool->GetId(), (LPARAM)MAKELONG(enable, 0));
}

void wxToolBar::DoToggleTool(wxToolBarToolBase *tool, bool toggle)
{
    ::SendMessage(GetHwnd(), TB_CHECKBUTTON,
                  (WPARAM)tool->GetId(), (LPARAM)MAKELONG(toggle, 0));
}

void wxToolBar::DoSetToggle(wxToolBarToolBase *WXUNUSED(tool), bool WXUNUSED(toggle))
{
    // VZ: AFAIK, the button has to be created either with TBSTYLE_CHECK or
    //     without, so we really need to delete the button and recreate it here
    wxFAIL_MSG( wxT("not implemented") );
}

void wxToolBar::SetToolNormalBitmap( int id, const wxBitmap& bitmap )
{
    wxToolBarTool* tool = static_cast<wxToolBarTool*>(FindById(id));
    if ( tool )
    {
        wxCHECK_RET( tool->IsButton(), wxT("Can only set bitmap on button tools."));

        tool->SetNormalBitmap(bitmap);
        Realize();
    }
}

void wxToolBar::SetToolDisabledBitmap( int id, const wxBitmap& bitmap )
{
    wxToolBarTool* tool = static_cast<wxToolBarTool*>(FindById(id));
    if ( tool )
    {
        wxCHECK_RET( tool->IsButton(), wxT("Can only set bitmap on button tools."));

        tool->SetDisabledBitmap(bitmap);
        Realize();
    }
}

// ----------------------------------------------------------------------------
// event handlers
// ----------------------------------------------------------------------------

// Responds to colour changes, and passes event on to children.
void wxToolBar::OnSysColourChanged(wxSysColourChangedEvent& event)
{
    wxRGBToColour(m_backgroundColour, ::GetSysColor(COLOR_BTNFACE));

    // Remap the buttons
    Realize();

    // Relayout the toolbar
    int nrows = m_maxRows;
    m_maxRows = 0;      // otherwise SetRows() wouldn't do anything
    SetRows(nrows);

    Refresh();

    // let the event propagate further
    event.Skip();
}

void wxToolBar::OnMouseEvent(wxMouseEvent& event)
{
    if ( event.Leaving() )
    {
        if ( m_pInTool )
        {
            OnMouseEnter(wxID_ANY);
            m_pInTool = NULL;
        }

        event.Skip();
        return;
    }

    if ( event.RightDown() )
    {
        // find the tool under the mouse
        wxCoord x = 0, y = 0;
        event.GetPosition(&x, &y);

        wxToolBarToolBase *tool = FindToolForPosition(x, y);
        OnRightClick(tool ? tool->GetId() : -1, x, y);
    }
    else
    {
        event.Skip();
    }
}

// This handler is required to allow the toolbar to be set to a non-default
// colour: for example, when it must blend in with a notebook page.
void wxToolBar::OnEraseBackground(wxEraseEvent& event)
{
    RECT rect = wxGetClientRect(GetHwnd());

    wxDC *dc = event.GetDC();
    HDC hdc = GetHdcOf(*dc);

#if wxUSE_UXTHEME
    // we may need to draw themed colour so that we appear correctly on
    // e.g. notebook page under XP with themes but only do it if the parent
    // draws themed background itself
    if ( !UseBgCol() && !GetParent()->UseBgCol() )
    {
        wxUxThemeEngine *theme = wxUxThemeEngine::GetIfActive();
        if ( theme )
        {
            HRESULT
                hr = theme->DrawThemeParentBackground(GetHwnd(), hdc, &rect);
            if ( hr == S_OK )
                return;

            // it can also return S_FALSE which seems to simply say that it
            // didn't draw anything but no error really occurred
            if ( FAILED(hr) )
            {
                wxLogApiError(wxT("DrawThemeParentBackground(toolbar)"), hr);
            }
        }
    }

    if ( MSWEraseRect(*dc) )
        return;
#endif // wxUSE_UXTHEME

    // we need to always draw our background under XP, as otherwise it doesn't
    // appear correctly with some themes (e.g. Zune one)
    if ( wxGetWinVersion() == wxWinVersion_XP ||
            UseBgCol() || (GetMSWToolbarStyle() & TBSTYLE_TRANSPARENT) )
    {
        // do draw our background
        //
        // notice that this 'dumb' implementation may cause flicker for some of
        // the controls in which case they should intercept wxEraseEvent and
        // process it themselves somehow
        AutoHBRUSH hBrush(wxColourToRGB(GetBackgroundColour()));

        wxCHANGE_HDC_MAP_MODE(hdc, MM_TEXT);
        ::FillRect(hdc, &rect, hBrush);
    }
    else // we have no non-default background colour
    {
        // let the system do it for us
        event.Skip();
    }
}

bool wxToolBar::HandleSize(WXWPARAM WXUNUSED(wParam), WXLPARAM lParam)
{
    // wait until we have some tools
    if ( !GetToolsCount() )
        return false;

    // calculate our minor dimension ourselves - we're confusing the standard
    // logic (TB_AUTOSIZE) with our horizontal toolbars and other hacks
    const RECT r = wxGetTBItemRect(GetHwnd(), 0);
    if ( !r.right )
        return false;

    int w, h;

    if ( IsVertical() )
    {
        w = r.right - r.left;
        if ( m_maxRows )
        {
            w *= (m_nButtons + m_maxRows - 1)/m_maxRows;
        }
        h = HIWORD(lParam);
    }
    else
    {
        w = LOWORD(lParam);
        if (HasFlag( wxTB_FLAT ))
            h = r.bottom - r.top - 3;
        else
            h = r.bottom - r.top;
        if ( m_maxRows )
        {
            // FIXME: hardcoded separator line height...
            h += HasFlag(wxTB_NODIVIDER) ? 4 : 6;
            h *= m_maxRows;
        }
    }

    if ( MAKELPARAM(w, h) != lParam )
    {
        // size really changed
        SetSize(w, h);
    }

    UpdateStretchableSpacersSize();

    // message processed
    return true;
}

#ifndef __WXWINCE__

bool wxToolBar::MSWEraseRect(wxDC& dc, const wxRect *rectItem)
{
    // erase the given rectangle to hide the separator
#if wxUSE_UXTHEME
    // themed background doesn't look well under XP so only draw it for Vista
    // and later
    if ( !UseBgCol() && wxGetWinVersion() >= wxWinVersion_Vista )
    {
        wxUxThemeEngine *theme = wxUxThemeEngine::GetIfActive();
        if ( theme )
        {
            wxUxThemeHandle hTheme(this, L"REBAR");

            // Draw the whole background since the pattern may be position
            // sensitive; but clip it to the area of interest.
            RECT rcTotal;
            wxCopyRectToRECT(GetClientSize(), rcTotal);

            RECT rcItem;
            if ( rectItem )
                wxCopyRectToRECT(*rectItem, rcItem);

            HRESULT hr = theme->DrawThemeBackground
                                (
                                    hTheme,
                                    GetHdcOf(dc),
                                    0, 0,
                                    &rcTotal,
                                    rectItem ? &rcItem : NULL
                                );
            if ( hr == S_OK )
                return true;

            // it can also return S_FALSE which seems to simply say that it
            // didn't draw anything but no error really occurred
            if ( FAILED(hr) )
            {
                wxLogApiError(wxT("DrawThemeBackground(toolbar)"), hr);
            }
        }
    }
#endif // wxUSE_UXTHEME

    // this is a bit peculiar but we may simply do nothing here if no rectItem
    // is specified (and hence we need to erase everything) as this only
    // happens when we're called from OnEraseBackground() and in this case we
    // may simply return false to let the systems default background erasing to
    // take place
    if ( rectItem )
        dc.DrawRectangle(*rectItem);

    return false;
}

bool wxToolBar::HandlePaint(WXWPARAM wParam, WXLPARAM lParam)
{
    // erase any dummy separators which were used only for reserving space in
    // the toolbar (either for a control or just for a stretchable space)

    // first of all, are there any controls at all?
    wxToolBarToolsList::compatibility_iterator node;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarToolBase * const tool = node->GetData();
        if ( tool->IsControl() || tool->IsStretchableSpace() )
            break;
    }

    if ( !node )
    {
        // no controls, nothing to erase
        return false;
    }

    // prepare the DC on which we'll be drawing
    wxClientDC dc(this);
    dc.SetPen(*wxTRANSPARENT_PEN);

    RECT rcUpdate;
    if ( !::GetUpdateRect(GetHwnd(), &rcUpdate, FALSE) )
    {
        // nothing to redraw anyhow
        return false;
    }

    const wxRect rectUpdate = wxRectFromRECT(rcUpdate);
    dc.SetClippingRegion(rectUpdate);

    // draw the toolbar tools, separators &c normally
    wxControl::MSWWindowProc(WM_PAINT, wParam, lParam);

    // for each control in the toolbar find all the separators intersecting it
    // and erase them
    //
    // NB: this is really the only way to do it as we don't know if a separator
    //     corresponds to a control (i.e. is a dummy one) or a real one
    //     otherwise
    int toolIndex = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext(), toolIndex++ )
    {
        wxToolBarTool *tool = (wxToolBarTool*)node->GetData();
        if ( tool->IsControl() )
        {
            // get the control rect in our client coords
            wxControl *control = tool->GetControl();
            wxStaticText *staticText = tool->GetStaticText();
            wxRect rectCtrl = control->GetRect();
            wxRect rectStaticText;
            if ( staticText )
                rectStaticText = staticText->GetRect();

            if ( !rectCtrl.Intersects(rectUpdate) &&
                    (!staticText || !rectStaticText.Intersects(rectUpdate)) )
                continue;

            // iterate over all buttons to find all separators intersecting
            // this control
            TBBUTTON tbb;
            int count = ::SendMessage(GetHwnd(), TB_BUTTONCOUNT, 0, 0);
            for ( int n = 0; n < count; n++ )
            {
                // is it a separator?
                if ( !::SendMessage(GetHwnd(), TB_GETBUTTON,
                                    n, (LPARAM)&tbb) )
                {
                    wxLogDebug(wxT("TB_GETBUTTON failed?"));

                    continue;
                }

                if ( tbb.fsStyle != TBSTYLE_SEP )
                    continue;

                // get the bounding rect of the separator
                RECT r = wxGetTBItemRect(GetHwnd(), n);
                if ( !r.right )
                    continue;

                const wxRect rectItem = wxRectFromRECT(r);

                // does it intersect the update region at all?
                if ( !rectUpdate.Intersects(rectItem) )
                    continue;

                // does it intersect the control itself or its label?
                //
                // if it does, refresh it so it's redrawn on top of the
                // background
                if ( rectCtrl.Intersects(rectItem) )
                    control->Refresh(false);
                else if ( staticText && rectStaticText.Intersects(rectItem) )
                    staticText->Refresh(false);
                else
                    continue;

                MSWEraseRect(dc, &rectItem);
            }
        }
        else if ( tool->IsStretchableSpace() )
        {
            const wxRect
                rectItem = wxRectFromRECT(wxGetTBItemRect(GetHwnd(), toolIndex));

            if ( rectUpdate.Intersects(rectItem) )
                MSWEraseRect(dc, &rectItem);
        }
    }

    return true;
}
#endif // __WXWINCE__

void wxToolBar::HandleMouseMove(WXWPARAM WXUNUSED(wParam), WXLPARAM lParam)
{
    wxCoord x = GET_X_LPARAM(lParam),
            y = GET_Y_LPARAM(lParam);
    wxToolBarToolBase* tool = FindToolForPosition( x, y );

    // has the current tool changed?
    if ( tool != m_pInTool )
    {
        m_pInTool = tool;
        OnMouseEnter(tool ? tool->GetId() : wxID_ANY);
    }
}

WXLRESULT wxToolBar::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    switch ( nMsg )
    {
        case WM_MOUSEMOVE:
            // we don't handle mouse moves, so always pass the message to
            // wxControl::MSWWindowProc (HandleMouseMove just calls OnMouseEnter)
            HandleMouseMove(wParam, lParam);
            break;

        case WM_SIZE:
            if ( HandleSize(wParam, lParam) )
                return 0;
            break;

#ifndef __WXWINCE__
        case WM_PAINT:
            // refreshing the controls in the toolbar inside a composite window
            // results in an endless stream of WM_PAINT messages -- and seems
            // to be unnecessary anyhow as everything works just fine without
            // any special workarounds in this case
            if ( !IsDoubleBuffered() && HandlePaint(wParam, lParam) )
                return 0;
            break;
#endif // __WXWINCE__
    }

    return wxControl::MSWWindowProc(nMsg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// private functions
// ----------------------------------------------------------------------------

#ifdef wxREMAP_BUTTON_COLOURS

WXHBITMAP wxToolBar::MapBitmap(WXHBITMAP bitmap, int width, int height)
{
    MemoryHDC hdcMem;

    if ( !hdcMem )
    {
        wxLogLastError(wxT("CreateCompatibleDC"));

        return bitmap;
    }

    SelectInHDC bmpInHDC(hdcMem, (HBITMAP)bitmap);

    if ( !bmpInHDC )
    {
        wxLogLastError(wxT("SelectObject"));

        return bitmap;
    }

    wxCOLORMAP *cmap = wxGetStdColourMap();

    for ( int i = 0; i < width; i++ )
    {
        for ( int j = 0; j < height; j++ )
        {
            COLORREF pixel = ::GetPixel(hdcMem, i, j);

            for ( size_t k = 0; k < wxSTD_COL_MAX; k++ )
            {
                COLORREF col = cmap[k].from;
                if ( abs(GetRValue(pixel) - GetRValue(col)) < 10 &&
                     abs(GetGValue(pixel) - GetGValue(col)) < 10 &&
                     abs(GetBValue(pixel) - GetBValue(col)) < 10 )
                {
                    if ( cmap[k].to != pixel )
                        ::SetPixel(hdcMem, i, j, cmap[k].to);
                    break;
                }
            }
        }
    }

    return bitmap;
}

#endif // wxREMAP_BUTTON_COLOURS

#endif // wxUSE_TOOLBAR