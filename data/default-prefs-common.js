// mouse wheel: disable mozilla's ctrl+wheel zooming, and enable our own
// set numlines to -1 to reverse directions, see bug #306110
pref("mousewheel.withcontrolkey.action", 0);
// NOTE! on gecko 1.9 we must set this to 1, see https://bugzilla.mozilla.org/show_bug.cgi?id=141476
pref("mousewheel.withcontrolkey.numlines", -1);
pref("mousewheel.withcontrolkey.sysnumlines", false);

// fix horizontal scroll with 2nd wheel, see bug #148557
pref("mousewheel.horizscroll.withnokey.action", 0);
pref("mousewheel.horizscroll.withnokey.sysnumlines", true);

// don't allow opening file:/// URLs on pages from network sources (http, etc.)
pref("security.checkloaduri", true);

// enable line wrapping in View Source
pref("view_source.wrap_long_lines", true);

// disable sidebar What's Related, we don't use it
pref("browser.related.enabled", false);

// Work around for mozilla focus bugs
pref("mozilla.widget.raise-on-setfocus", false);

// ftp
pref("advanced.mailftp", false);
// disable sucky XUL ftp view, have nice ns4-like html page instead
pref("network.dir.generate_html", true);

// deactivate mailcap support, it breaks Gnome-based helper apps
pref("helpers.global_mailcap_file", "");
pref("helpers.private_mailcap_file", "");

// use the mozilla defaults for mime.types files to let mozilla guess proper
// Content-Type for file uploads instead of always falling back to
// application/octet-stream
pref("helpers.global_mime_types_file", "");
pref("helpers.private_mime_types_file", "");

// enable keyword search
pref("keyword.enabled", true);

// disable usless security warnings
pref("security.warn_entering_secure", false);
pref("security.warn_entering_secure.show_once", true);
pref("security.warn_leaving_secure", false);
pref("security.warn_leaving_secure.show_once", false);
pref("security.warn_submit_insecure", false);
pref("security.warn_submit_insecure.show_once", false);
pref("security.warn_viewing_mixed", true);
pref("security.warn_viewing_mixed.show_once", false);
pref("security.warn_entering_weak", true);
pref("security.warn_entering_weak.show_once", false);

// always ask which client cert to use
pref("security.default_personal_cert", "Ask Every Time");

// fonts
pref("font.size.unit", "pt");

// protocols
pref("network.protocol-handler.external.ftp", false);
pref("network.protocol-handler.external.irc", true);
pref("network.protocol-handler.external.mailto", true);
pref("network.protocol-handler.external.news", true);
pref("network.protocol-handler.external.nntp", true);
pref("network.protocol-handler.external.snews", true);
pref("network.protocol-handler.external.webcal", true);
// but don't show warnings for these
pref("network.protocol-handler.warn-external.irc", false);
pref("network.protocol-handler.warn-external.mailto", false);
pref("network.protocol-handler.warn-external.news", false);
pref("network.protocol-handler.warn-external.nntp", false);
pref("network.protocol-handler.warn-external.snews", false);
pref("network.protocol-handler.warn-external.webcal", false);

// don't warn
pref("network.protocol-handler.warn-external.rtsp", false);
pref("network.protocol-handler.warn-external.mms", false);
pref("network.protocol-handler.warn-external.mmsh", false);

// disable xpinstall
pref("xpinstall.enabled", false);

// enable plugin finder
pref("plugin.default_plugin_disabled", true);

// enable locale matching
pref("intl.locale.matchOS", true);

// enable fixed-up typeaheadfind extension
pref("accessibility.typeaheadfindsea", false);
pref("accessibility.typeaheadfindsea.autostart", true);
pref("accessibility.typeaheadfindsea.linksonly", true);

// disable image resizing
pref("browser.enable_automatic_image_resizing", false);

// enable password manager
// need to include those prefs since xulrunner doesn't include them
pref("signon.rememberSignons", true);
pref("signon.expireMasterPassword", false);
pref("signon.SignonFileName", "signons.txt");
pref("signon.SignonFileName2", "signons2.txt");
pref("signon.autofillForms", true);

// use system colours
pref("browser.display.use_system_colors", true);

// explicitly enable error pages (xulrunner is missing this pref)
pref("browser.xul.error_pages.enabled", true);

// unset weird xulrunner default UA string
pref("general.useragent.extra.simple", "");
pref("general.useragent.vendor", "");
pref("general.useragent.vendorComment", "");
pref("general.useragent.vendorSub", "");

// we don't want ping(uin)s
pref("browser.send_pings", false);
pref("browser.send_pings.require_same_host", true);

// disable blink tags
pref("browser.blink_allowed", false);

// enable spatial navigation (only works if the extension is built with gecko)
pref("snav.enabled", true);

// don't leak UI language, and don't default to "en-US"
pref("general.useragent.locale", "en");

// spellcheck
// pref("extensions.spellcheck.inline.max-misspellings", -1);
// 0: disabled, 1: only textareas, 2: check textareas and single-line input fields
pref("layout.spellcheckDefault", 0);
pref("spellchecker.dictionary", "");

// print settings
pref("print.print_printer", "PostScript/default");
pref("print.use_global_printsettings", false);
pref("print.save_print_settings", false);
pref("print.show_print_progress", true);
pref("print.printer_list", "");
pref("print.postscript.enabled", true);
pref("print.postscript.cups.enabled", false);
// deactivate PAPI too (will be added in https://bugzilla.mozilla.org/show_bug.cgi?id=317450)
pref("print.postscript.papi.enabled", false);

// print margins used for the page header and footer, measured in hundredths of an inch
// see http://bugzilla.gnome.org/show_bug.cgi?id=346110
pref("print.printer_PostScript/default.print_edge_bottom", 15);
pref("print.printer_PostScript/default.print_edge_left", 15);
pref("print.printer_PostScript/default.print_edge_right", 15);
pref("print.printer_PostScript/default.print_edge_top", 15);

// sites stealthly storing unlimited amount of data? no thanks
pref("dom.storage.enabled", false);

// warn about about:config
pref("general.warnOnAboutConfig", false);

// enable negotiate extension
pref("network.negotiate-auth.trusted-uris", "https://");

// JS annoyances
pref("dom.disable_window_flip", true);

// Whether delete and backspace should immediately delete characters not
// visually adjacent to the caret, or adjust the visual position of the caret
// on the first keypress and delete the character on a second keypress
// (added in bug https://bugzilla.mozilla.org/show_bug.cgi?id=328834)
// set to true for parity with gtk entries
pref("bidi.edit.delete_immediately", true);
