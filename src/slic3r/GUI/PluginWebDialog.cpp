#include "PluginWebDialog.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"

#include <libslic3r/Utils.hpp>
#include "slic3r/plugin/PluginManager.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/event.h>
#include <wx/filename.h>

#ifdef __WIN32__
#include <WebView2.h>
#include <objbase.h>
#ifdef __VISUALC__
#include <wrl/event.h>
using Microsoft::WRL::Callback;
#else
#include <wx/msw/private/comptr.h>
#include <wx/msw/wrl/event.h>
#endif
#endif

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef __linux__
#include <webkit2/webkit2.h>

// Signal names for glib
#define DOWNLOAD_STARTED_SIGNAL "download-started"
#define DOWNLOAD_DESTINATION_SIGNAL "decide-destination"
#define DOWNLOAD_FINISHED_SIGNAL "finished"
#define DOWNLOAD_FAILED_SIGNAL "failed"
#define DOWNLOAD_POLICY_SIGNAL "decide-policy"
#endif

namespace Slic3r { namespace GUI {

wxString sanitize_plugin_download_filename(const wxString& suggested)
{
    wxString name = wxFileName(suggested).GetFullName();
    for (size_t i = 0; i < name.length(); ++i) {
        if (name[i] < 0x20 || name[i] == '/' || name[i] == '\\' || name[i] == ':' || name[i] == '<' ||
            name[i] == '>' || name[i] == '"' || name[i] == '|' || name[i] == '?' || name[i] == '*')
            name[i] = '_';
    }
    while (!name.empty() && (name.Last() == ' ' || name.Last() == '.'))
        name.RemoveLast();

#ifdef __WIN32__
    const wxString stem = wxFileName(name).GetName().Upper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.length() == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) && stem[3] >= '1' && stem[3] <= '9'))
        name.Prepend('_');
#endif

    return name.empty() ? wxString("download") : name;
}

boost::filesystem::path unique_plugin_download_path(const boost::filesystem::path& dir,
                                                     const boost::filesystem::path& filename)
{
    boost::filesystem::path candidate = dir / filename;
#ifdef __WIN32__
    const std::wstring stem = filename.stem().wstring();
    const std::wstring ext  = filename.extension().wstring();
    for (int n = 1; boost::filesystem::exists(candidate); ++n)
        candidate = dir / (stem + L" (" + std::to_wstring(n) + L")" + ext);
#else
    const std::string stem = filename.stem().string();
    const std::string ext  = filename.extension().string();
    for (int n = 1; boost::filesystem::exists(candidate); ++n)
        candidate = dir / (stem + " (" + std::to_string(n) + ")" + ext);
#endif
    return candidate;
}

void notify_plugin_download(PluginDownloadSession& session, nlohmann::json info)
{
    if (session.notified)
        return;
    session.notified = true;
    if (session.callback) {
        try {
            session.callback(info);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "PluginWebDialog: download callback failed: " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "PluginWebDialog: download callback failed.";
        }
    }
}

void finish_plugin_download(PluginDownloadSession& session, long long size)
{
    notify_plugin_download(session,
                           {{"filename", std::string(session.resolved_filename.utf8_string())},
                            {"path", std::string(session.resolved_path.utf8_string())},
                            {"mimeType", std::string(session.mime_type.utf8_string())},
                            {"size", size},
                            {"success", true},
                            {"error", ""}});
}

void fail_plugin_download(PluginDownloadSession& session, const std::string& error)
{
    notify_plugin_download(session,
                           {{"filename", std::string(session.resolved_filename.utf8_string())},
                            {"path", ""},
                            {"mimeType", std::string(session.mime_type.utf8_string())},
                            {"size", 0},
                            {"success", false},
                            {"error", error}});
}

namespace {

// Low-specificity element defaults (no !important) for UNSTYLED plugin HTML, so a bare
// plugin page looks native while any CSS the plugin ships still wins. Built on the
// --orca-* variables the host injects (see WebViewHostDialog); document-start injected
// AFTER the host contract so the variables are defined (shares the base injector's
// WebView2 timing guard).
std::string plugin_defaults_user_script()
{
    std::string css;
    css += "<style id=\"orca-plugin-defaults\">";
    css += "html,body{background:var(--orca-bg);color:var(--orca-fg);"
           "font-family:var(--orca-font);font-size:13px;}";
    css += "body{margin:0;}";
    css += "h1,h2,h3,h4,h5,h6{color:var(--orca-fg);font-weight:600;}";
    css += "a{color:var(--orca-accent);}";
    css += "hr{border:0;border-top:1px solid var(--orca-border);}";
    css += "button{font:inherit;color:var(--orca-accent-fg);background:var(--orca-accent);"
           "border:1px solid var(--orca-accent);border-radius:4px;padding:5px 14px;cursor:pointer;}";
    css += "button:hover{filter:brightness(1.1);}";
    css += "button:disabled{opacity:.5;cursor:default;}";
    css += "input,select,textarea{font:inherit;color:var(--orca-fg);"
           "background:var(--orca-bg);border:1px solid var(--orca-border);"
           "border-radius:4px;padding:4px 8px;}";
    css += "input:focus,select:focus,textarea:focus{outline:none;border-color:var(--orca-accent);}";
    css += "table{border-collapse:collapse;}";
    css += "th,td{text-align:left;padding:6px 10px;border-bottom:1px solid var(--orca-border);}";
    css += "th{color:var(--orca-muted);font-weight:600;}";
    css += "::-webkit-scrollbar{width:12px;height:12px;}";
    css += "::-webkit-scrollbar-thumb{background:var(--orca-border);border-radius:6px;}";
    css += "::-webkit-scrollbar-track{background:transparent;}";
    css += "</style>";
    return WebViewHostDialog::document_start_injector(css, "orca-plugin-defaults", "beforeend");
}

// Injected into the top-level page at document start (before the plugin's own
// scripts). Defines window.orca as the only host surface the page may use. It
// references window.wx lazily (at call time) so it never races the backend's
// deferred registration of the "wx" message handler. Guarded against
// double-injection so it is harmless if also prepended.
constexpr char ORCA_BRIDGE_JS[] = R"JS(
(function () {
  if (window.top !== window.self) return;
  if (window.orca) return;
  var handlers = [];
  function send(kind, data) {
    try {
      window.wx.postMessage(JSON.stringify({
        channel: 'orca', kind: kind, data: (data === undefined ? null : data)
      }));
    } catch (e) { /* bridge not ready yet */ }
  }
  window.orca = {
    postMessage:         function (d) { send('message', d); },
    submit:              function (d) { send('submit', d); },
    close:               function ()  { send('close'); },
    onMessage:           function (cb) { if (typeof cb === 'function') handlers.push(cb); }
  };
  window.__orcaDispatch = function (payload) {
    var data = payload ? payload.data : null;
    for (var i = 0; i < handlers.length; i++) {
      try { handlers[i](data); } catch (e) {}
    }
  };
})();
)JS";

// file:// base URL for plugin HTML loaded via SetPage, so self-referencing
// relative URLs resolve against the bundled web resources directory.
wxString web_base_url()
{
    const std::string dir = (boost::filesystem::path(resources_dir()) / "web").make_preferred().string();
    return wxString("file://") + from_u8(dir) + "/";
}

#if defined(__linux__)

constexpr const char* PLUGIN_DOWNLOAD_REDIRECT_DATA_KEY = "orca-plugin-download-redirect-config";
constexpr const char* PLUGIN_DOWNLOAD_STARTED_KEY       = "orca-plugin-download-started-connected";
constexpr const char* PLUGIN_DOWNLOAD_POLICY_KEY        = "orca-plugin-download-policy-connected";

gboolean on_plugin_download_decide_destination(WebKitDownload* download, const gchar* suggested_filename, gpointer user_data)
{
    auto* session               = static_cast<PluginDownloadSession*>(user_data);
    const wxString safe_name    = sanitize_plugin_download_filename(wxString::FromUTF8(suggested_filename ? suggested_filename : ""));
    if (session->target_dir.empty()) {
        fail_plugin_download(*session, "Plugin storage directory is unavailable.");
        webkit_download_cancel(download);
        return TRUE;
    }

    namespace fs = boost::filesystem;
    const fs::path dir(session->target_dir.utf8_string());
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        fail_plugin_download(*session, "Could not create the plugin storage directory.");
        webkit_download_cancel(download);
        return TRUE;
    }

    const fs::path dest = unique_plugin_download_path(dir, fs::path(std::string(safe_name.utf8_string())));
    GError* uri_error   = nullptr;
    gchar* uri          = g_filename_to_uri(dest.string().c_str(), nullptr, &uri_error);
    if (!uri) {
        const std::string error = (uri_error && uri_error->message) ? uri_error->message :
                                                                      "Could not build a destination path for the download.";
        fail_plugin_download(*session, error);
        webkit_download_cancel(download);
        if (uri_error)
            g_error_free(uri_error);
        return TRUE;
    }

    session->resolved_filename = safe_name;
    session->resolved_path     = wxString::FromUTF8(dest.string());
    webkit_download_set_destination(download, uri);
    g_free(uri);
    return TRUE;
}

void on_plugin_download_finished(WebKitDownload* download, gpointer user_data)
{
    auto* session               = static_cast<PluginDownloadSession*>(user_data);
    WebKitURIResponse* response = webkit_download_get_response(download);
    const gchar* mime           = response ? webkit_uri_response_get_mime_type(response) : nullptr;
    session->mime_type = wxString::FromUTF8(mime ? mime : "");
    finish_plugin_download(*session, webkit_download_get_received_data_length(download));
    delete session;
}

void on_plugin_download_failed(WebKitDownload*, GError* error, gpointer user_data)
{
    auto* session = static_cast<PluginDownloadSession*>(user_data);
    fail_plugin_download(*session, (error && error->message) ? error->message : "Download failed.");
    delete session;
}

void on_plugin_download_started(WebKitWebContext*, WebKitDownload* download, gpointer)
{
    WebKitWebView* source_view = webkit_download_get_web_view(download);
    if (!source_view)
        return;

    auto* config = static_cast<PluginDownloadRedirectConfig*>(g_object_get_data(G_OBJECT(source_view), PLUGIN_DOWNLOAD_REDIRECT_DATA_KEY));
    if (!config)
        return;

    auto* session = new PluginDownloadSession{config->target_dir, config->callback};
    g_signal_connect(download, DOWNLOAD_DESTINATION_SIGNAL, G_CALLBACK(on_plugin_download_decide_destination), session);
    g_signal_connect(download, DOWNLOAD_FINISHED_SIGNAL, G_CALLBACK(on_plugin_download_finished), session);
    g_signal_connect(download, DOWNLOAD_FAILED_SIGNAL, G_CALLBACK(on_plugin_download_failed), session);
}

gboolean on_plugin_decide_policy(WebKitWebView* web_view,
                                 WebKitPolicyDecision* decision,
                                 WebKitPolicyDecisionType decision_type,
                                 gpointer)
{
    if (decision_type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE ||
        !g_object_get_data(G_OBJECT(web_view), PLUGIN_DOWNLOAD_REDIRECT_DATA_KEY))
        return FALSE;

    auto* response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
    if (webkit_response_policy_decision_is_mime_type_supported(response))
        return FALSE;

    // Binary responses without Content-Disposition can otherwise navigate the
    // page instead of becoming WebKitDownload objects.
    webkit_policy_decision_download(decision);
    return TRUE;
}

void enable_plugin_download_redirect(wxWebView* view, wxString target_dir, PluginDownloadCallback callback)
{
    auto* native_view = static_cast<WebKitWebView*>(view->GetNativeBackend());
    if (!native_view)
        return;

    auto* config = new PluginDownloadRedirectConfig{std::move(target_dir), std::move(callback)};
    g_object_set_data_full(G_OBJECT(native_view), PLUGIN_DOWNLOAD_REDIRECT_DATA_KEY, config,
                           [](gpointer data) { delete static_cast<PluginDownloadRedirectConfig*>(data); });

    WebKitWebContext* context = webkit_web_view_get_context(native_view);
    if (!g_object_get_data(G_OBJECT(context), PLUGIN_DOWNLOAD_STARTED_KEY)) {
        g_signal_connect(context, DOWNLOAD_STARTED_SIGNAL, G_CALLBACK(on_plugin_download_started), nullptr);
        g_object_set_data(G_OBJECT(context), PLUGIN_DOWNLOAD_STARTED_KEY, GUINT_TO_POINTER(1));
    }
    if (!g_object_get_data(G_OBJECT(native_view), PLUGIN_DOWNLOAD_POLICY_KEY)) {
        g_signal_connect(native_view, DOWNLOAD_POLICY_SIGNAL, G_CALLBACK(on_plugin_decide_policy), nullptr);
        g_object_set_data(G_OBJECT(native_view), PLUGIN_DOWNLOAD_POLICY_KEY, GUINT_TO_POINTER(1));
    }
}
#elif defined(__WIN32__)

HRESULT on_plugin_download_state_changed(const std::shared_ptr<PluginDownloadSession>& session,
                                          ICoreWebView2DownloadOperation* operation)
{
    try {
        COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
        if (!operation || FAILED(operation->get_State(&state)))
            return S_OK;
        if (state == COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS)
            return S_OK;

        INT64 bytes_received = 0;
        operation->get_BytesReceived(&bytes_received);
        LPWSTR result_path = nullptr;
        operation->get_ResultFilePath(&result_path);
        if (result_path) {
            session->resolved_path = wxString(result_path);
            CoTaskMemFree(result_path);
        }

        if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
            finish_plugin_download(*session, bytes_received);
        } else {
            COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason =
                COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
            operation->get_InterruptReason(&reason);
            fail_plugin_download(*session, "Download interrupted (" + std::to_string(static_cast<int>(reason)) + ").");
        }
    } catch (const std::exception& e) {
        fail_plugin_download(*session, e.what());
    } catch (...) {
        fail_plugin_download(*session, "Download failed.");
    }
    return S_OK;
}

void cancel_plugin_download(ICoreWebView2DownloadStartingEventArgs* args, ICoreWebView2DownloadOperation* operation)
{
    if (args)
        args->put_Cancel(TRUE);
    if (args)
        args->put_Handled(TRUE);
    if (operation)
        operation->Cancel();
}

HRESULT on_plugin_download_starting(ICoreWebView2DownloadStartingEventArgs* args,
                                    const std::shared_ptr<PluginDownloadRedirectConfig>& config)
{
    ICoreWebView2DownloadOperation* operation = nullptr;
    auto                             session  = std::make_shared<PluginDownloadSession>();
    session->target_dir                       = config->target_dir;
    session->callback                         = config->callback;

    try {
        if (!args || FAILED(args->get_DownloadOperation(&operation)))
            throw std::runtime_error("Could not initialize the download operation.");

        LPWSTR suggested_path = nullptr;
        args->get_ResultFilePath(&suggested_path);
        const wxString filename = sanitize_plugin_download_filename(suggested_path ? wxString(suggested_path) : wxString());
        if (suggested_path)
            CoTaskMemFree(suggested_path);
        session->resolved_filename = filename;

        LPWSTR mime_type = nullptr;
        if (SUCCEEDED(operation->get_MimeType(&mime_type)) && mime_type) {
            session->mime_type = wxString(mime_type);
            CoTaskMemFree(mime_type);
        }

        if (session->target_dir.empty())
            throw std::runtime_error("Plugin storage directory is unavailable.");

        namespace fs = boost::filesystem;
        const fs::path dir(std::wstring(session->target_dir.wc_str()));
        fs::create_directories(dir);
        if (!fs::is_directory(dir))
            throw std::runtime_error("Could not create the plugin storage directory.");

        const fs::path destination = unique_plugin_download_path(dir, fs::path(std::wstring(filename.wc_str())));
        session->resolved_path                       = wxString(destination.wstring());
        if (FAILED(args->put_ResultFilePath(destination.wstring().c_str())))
            throw std::runtime_error("Could not set the plugin download destination.");

        auto state_handler = Callback<ICoreWebView2StateChangedEventHandler>(
            [session](ICoreWebView2DownloadOperation* download, IUnknown*) {
                return on_plugin_download_state_changed(session, download);
            });
        EventRegistrationToken token{};
        const HRESULT state_result = operation->add_StateChanged(state_handler.Get(), &token);
        if (FAILED(state_result))
            throw std::runtime_error("Could not monitor the plugin download.");
    } catch (const std::exception& e) {
        cancel_plugin_download(args, operation);
        fail_plugin_download(*session, e.what());
    } catch (...) {
        cancel_plugin_download(args, operation);
        fail_plugin_download(*session, "Could not redirect the plugin download.");
    }

    if (operation)
        operation->Release();
    return S_OK;
}

void attach_plugin_download_redirect(wxWebView* view, const std::shared_ptr<PluginDownloadRedirectConfig>& config)
{
    auto* web_view = static_cast<ICoreWebView2*>(view->GetNativeBackend());
    if (!web_view)
        throw std::runtime_error("WebView2 is not ready for download redirection.");

    ICoreWebView2_4* web_view_4 = nullptr;
    const HRESULT query_result = web_view->QueryInterface(IID_ICoreWebView2_4,
                                                           reinterpret_cast<void**>(&web_view_4));
    if (FAILED(query_result) || !web_view_4)
        throw std::runtime_error("WebView2 download interception is unavailable.");

    auto handler = Callback<ICoreWebView2DownloadStartingEventHandler>(
        [config](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
            try {
                return on_plugin_download_starting(args, config);
            } catch (...) {
                // COM event callbacks must not allow exceptions to escape into WebView2.
                if (args)
                    args->put_Cancel(TRUE);
                return E_FAIL;
            }
        });
    EventRegistrationToken token{};
    const HRESULT add_result = web_view_4->add_DownloadStarting(handler.Get(), &token);
    web_view_4->Release();
    if (FAILED(add_result))
        throw std::runtime_error("Could not register WebView2 download interception.");
}

void enable_plugin_download_redirect(wxWebView* view, wxString target_dir, PluginDownloadCallback callback)
{
    if (!view)
        return;

    auto config = std::make_shared<PluginDownloadRedirectConfig>(PluginDownloadRedirectConfig{std::move(target_dir), std::move(callback)});
    if (view->GetNativeBackend()) {
        attach_plugin_download_redirect(view, config);
        return;
    }

    // WebView2 creates its native controller asynchronously. The created event is
    // queued by wxWidgets after GetNativeBackend() becomes usable.
    view->Bind(wxEVT_WEBVIEW_CREATED, [view, config](wxWebViewEvent&) {
        try {
            attach_plugin_download_redirect(view, config);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "PluginWebDialog: " << e.what();
        }
    });
}
#elif defined(__WXMAC__) || defined(__WXOSX__)

void enable_plugin_download_redirect(wxWebView* view, wxString target_dir, PluginDownloadCallback callback)
{
    if (!view || !view->GetNativeBackend())
        return;

    WKWebView_setDownloadRedirect(
        view->GetNativeBackend(),
        std::move(target_dir),
        std::move(callback));
}
#endif

} // namespace

PluginWebDialog::PluginWebDialog(wxWindow* parent,
                                 const wxString& title,
                                 const std::string& plugin_key,
                                 const std::string& html,
                                 const std::string& url,
                                 const wxSize& size,
                                 MessageHandler on_message,
                                 SubmitHandler on_submit,
                                 DownloadHandler on_download_complete,
                                 CloseHandler on_close,
                                 CloseHandler on_destroyed,
                                 long wx_style)
    : WebViewHostDialog(parent, wxID_ANY, title, wxDefaultPosition, size, wx_style)
    , m_html(html)
    , m_url(url)
    , m_plugin_key(plugin_key)
    , m_on_message(std::move(on_message))
    , m_on_submit(std::move(on_submit))
    , m_on_close(std::move(on_close))
    , m_on_destroyed(std::move(on_destroyed))
{
    // A tiny bundled bootstrap page brings the webview up; the real plugin HTML
    // is swapped in via SetPage once the bootstrap finishes loading.
    create_webview("web/dialog/PluginWebDialog/blank.html", title, size, wxSize(320, 240));

    // Paint the window/webview in the themed background so there is no white
    // flash before the (transparent) bootstrap page and plugin HTML render.
    SetBackgroundColour(wxGetApp().get_window_default_clr());

    if (wxWebView* wv = browser()) {
        wv->SetBackgroundColour(wxGetApp().get_window_default_clr());
        // Theme contract + plugin defaults + bridge are registered by the base
        // create_webview() via add_user_scripts(); nothing to add here.
        // Swap in the plugin HTML once the bootstrap page settles. Bind ERROR too so a
        // missing/blocked bootstrap resource (e.g. a packaged build) still triggers it.
        Bind(wxEVT_WEBVIEW_LOADED, &PluginWebDialog::on_bootstrap_event, this, wv->GetId());
        Bind(wxEVT_WEBVIEW_ERROR, &PluginWebDialog::on_bootstrap_event, this, wv->GetId());

        wxString storage_dir;
        try {
            storage_dir = wxString::FromUTF8(PluginManager::instance().get_storage_dir(m_plugin_key));
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "PluginWebDialog: get_storage_dir('" << m_plugin_key
                                       << "') failed, blocking downloads for this dialog: " << e.what();
        }
        try {
            enable_plugin_download_redirect(wv, std::move(storage_dir), std::move(on_download_complete));
        } catch (const std::runtime_error& e) {
            BOOST_LOG_TRIVIAL(warning) << "PluginWebDialog: " << e.what();
        }
    }
    Bind(wxEVT_CLOSE_WINDOW, &PluginWebDialog::on_close_window, this);
}

void PluginWebDialog::add_user_scripts()
{
    if (wxWebView* wv = browser()) {
        wv->AddUserScript(wxString::FromUTF8(plugin_defaults_user_script()));
        wv->AddUserScript(ORCA_BRIDGE_JS);
    }
}

PluginWebDialog::~PluginWebDialog()
{
    // Runs on every destruction path. Deliberately NOT a wxEVT_DESTROY handler:
    // that event is sent from the base ~wxDialog(), after this subclass's members
    // are already destroyed. Here the members are still alive, and the callback
    // only touches the host-side registry (no Python), so this is safe.
    if (m_on_destroyed)
        m_on_destroyed();
}

void PluginWebDialog::post_message(PluginWebDialog* dialog, const nlohmann::json& data)
{
    if (dialog != nullptr && dialog->is_open())
        dialog->push_message(data);
}

void PluginWebDialog::request_close(PluginWebDialog* dialog)
{
    if (dialog != nullptr)
        dialog->Close();
}

void PluginWebDialog::destroy_for_plugin(PluginWebDialog* dialog)
{
    if (dialog == nullptr)
        return;

    // Forced plugin teardown must not invoke Python close callbacks. End a modal
    // loop first, otherwise destroying the window can leave ShowModal() running.
    if (dialog->IsModal()) {
        dialog->m_open = false;
        dialog->EndModal(wxID_CANCEL);
    }
    dialog->Destroy();
}

void PluginWebDialog::on_bootstrap_event(wxWebViewEvent& event)
{
    // The first bootstrap load (or its error) triggers the swap to plugin HTML;
    // the resulting plugin-page load is ignored (guarded by m_content_loaded).
    load_plugin_content();
    event.Skip();
}

void PluginWebDialog::load_plugin_content()
{
    if (m_content_loaded)
        return;
    m_content_loaded = true;
    if (wxWebView* wv = browser()) {
        if (!m_url.empty())
            wv->LoadURL(wxString::FromUTF8(m_url));
        else
            wv->SetPage(wxString::FromUTF8(m_html), web_base_url());
    }
}

void PluginWebDialog::on_script_message(const nlohmann::json& payload)
{
    if (payload.value("channel", std::string()) == "orca") {
        const std::string kind    = payload.value("kind", std::string());
        const nlohmann::json data = payload.contains("data") ? payload["data"] : nlohmann::json();
        if (kind == "message") {
            if (m_on_message)
                m_on_message(data);
        } else if (kind == "submit") {
            finish(true, data);
        } else if (kind == "close") {
            finish(false, nlohmann::json());
        }
        return;
    }

    // Fall back to the shared shell commands (e.g. "close_page").
    handle_common_script_command(payload);
}

void PluginWebDialog::push_message(const nlohmann::json& data)
{
    if (!m_open)
        return;
    nlohmann::json envelope;
    envelope["data"] = data;
    call_web_handler(envelope, wxT("__orcaDispatch"));
}

void PluginWebDialog::finish(bool submitted, const nlohmann::json& data)
{
    if (!m_open)
        return;
    m_open = false;
    if (submitted) {
        m_result = data;
        fire_submit(data);
    } else {
        m_result.reset();
        fire_close();
    }

    if (IsModal())
        EndModal(submitted ? wxID_OK : wxID_CANCEL);
    else
        Close();
}

void PluginWebDialog::on_close_window(wxCloseEvent&)
{
    if (!m_open) {
        // finish() already dispatched submit/close and requested the close.
        // Modeless windows still need to be destroyed after that request.
        if (!IsModal())
            Destroy();
        return;
    }

    m_open = false;
    m_result.reset();
    fire_close();
    if (IsModal()) {
        EndModal(wxID_CANCEL);
        return;
    }
    Destroy();
}

void PluginWebDialog::fire_submit(const nlohmann::json& data)
{
    if (m_on_submit) {
        SubmitHandler cb = std::move(m_on_submit);
        cb(data);
    }
}

void PluginWebDialog::fire_close()
{
    if (m_close_fired)
        return;
    m_close_fired = true;
    if (m_on_close) {
        CloseHandler cb = m_on_close;
        m_on_close      = nullptr;
        cb();
    }
}

}} // namespace Slic3r::GUI
