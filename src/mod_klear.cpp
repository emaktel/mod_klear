// SPDX-License-Identifier: Apache-2.0
//
// mod_klear — FreeSWITCH module for AEC3-based echo cancellation and
// DeepFilterNet-based neural noise suppression.
//
// Enabling on a channel:
//
//   # dialplan
//   <action application="klear" data="start"/>
//
//   # fs_cli / ESL
//   klear <uuid> start
//   klear <uuid> stop
//   klear <uuid> set aec=on ns=off
//
// Channel variables (read at start and as hot toggles):
//
//   klear_aec        bool       default per global config, echo canceller
//   klear_ns         bool       default per global config, noise suppressor
//   klear_hpf        bool       default true,              high-pass filter
//   klear_ns_atten   float dB   default 30                 DF atten_lim_db
//
// The module attaches one media bug per channel with SMBF_READ_REPLACE |
// SMBF_WRITE_REPLACE. On WRITE_REPLACE frames (audio FreeSWITCH is about to
// send towards the remote leg) we feed AEC the far-end reference. On
// READ_REPLACE frames (audio just arrived from the remote leg) we run the
// AEC capture path, then DF noise suppression, writing the cleaned buffer
// back in place. Toggles are atomic and applied per frame without detaching
// the bug.

#include <switch.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "backends/df_ns.h"
#include "backends/null_ns.h"
#include "backends/webrtc_aec.h"
#include "processor.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_klear_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_klear_shutdown);
SWITCH_MODULE_DEFINITION(mod_klear, mod_klear_load, mod_klear_shutdown, nullptr);

namespace {

// ---- Globals ------------------------------------------------------------

// Canonical configuration presets. Each preset is a named bundle of
// backend settings tuned against the offline benchmark in test/reports/.
// Pick one via the klear_preset channel variable or global default; the
// individual klear_aec / klear_ns / klear_hpf / klear_ns_atten variables
// override any preset on the same channel.
enum class Preset {
    Agent,      // DF-only. Best near-end preservation on doubletalk
                // (AECMOS deg 3.26) while DF's neural discrimination
                // still removes ~17 dB of echo energy. Target: AI agents,
                // ASR feeds, anything where caller speech must survive.
    Telephony,  // AEC3 + DF aggressive. Maximum echo cancellation
                // (>38 dB far-end ERLE) at the cost of near-end
                // damage during doubletalk. Target: classic human-to-
                // human calls where echo is unforgivable.
    AecOnly,    // Raw WebRTC AEC3, no DF. Cheapest path; ~17 dB ERLE
                // with near-zero near-end damage in single-talk.
};

struct Globals {
    Preset default_preset = Preset::Agent;
    float default_ns_atten_db = 30.0f;
    bool default_hpf = true;
};

Globals g_globals{};

constexpr const char* kBugName = "klear";
constexpr const char* kChannelPrivateKey = "_klear_";

// ---- Per-channel state --------------------------------------------------

struct KlearSession {
    switch_core_session_t* fs_session = nullptr;
    switch_mutex_t* lock = nullptr;
    std::unique_ptr<klear::Processor> proc;
    int channel_rate = 0;
    bool ns_disabled_rate = false;
    std::uint64_t render_frames = 0;
    std::uint64_t capture_frames = 0;
};

void destroy_klear(KlearSession* ks) {
    if (!ks) return;
    ks->proc.reset();
    delete ks;
}

// ---- Channel variable helpers -------------------------------------------

bool var_bool(switch_channel_t* channel, const char* name, bool fallback) {
    const char* v = switch_channel_get_variable(channel, name);
    if (!v || !*v) return fallback;
    return switch_true(v);
}

float var_float(switch_channel_t* channel, const char* name, float fallback) {
    const char* v = switch_channel_get_variable(channel, name);
    if (!v || !*v) return fallback;
    return static_cast<float>(atof(v));
}

// ---- Media bug callback -------------------------------------------------

switch_bool_t klear_callback(switch_media_bug_t* bug, void* user_data,
                             switch_abc_type_t type) {
    auto* ks = static_cast<KlearSession*>(user_data);
    if (!ks) return SWITCH_FALSE;

    switch (type) {
    case SWITCH_ABC_TYPE_INIT:
        return SWITCH_TRUE;

    case SWITCH_ABC_TYPE_CLOSE: {
        switch_mutex_lock(ks->lock);
        switch_core_session_t* session = ks->fs_session;
        klear::ProcessorStats s{};
        if (ks->proc) ks->proc->get_stats(&s);
        switch_mutex_unlock(ks->lock);

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
            "klear: close. frames_capture=%llu frames_render=%llu "
            "cpu=%.3fs\n",
            static_cast<unsigned long long>(ks->capture_frames),
            static_cast<unsigned long long>(ks->render_frames),
            s.cpu_seconds_total);

        switch_event_t* event = nullptr;
        if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM,
                                         "klear::stats") ==
            SWITCH_STATUS_SUCCESS) {
            switch_channel_event_set_data(
                switch_core_session_get_channel(session), event);
            switch_event_add_header(event, SWITCH_STACK_BOTTOM,
                "klear-capture-frames", "%llu",
                static_cast<unsigned long long>(ks->capture_frames));
            switch_event_add_header(event, SWITCH_STACK_BOTTOM,
                "klear-render-frames", "%llu",
                static_cast<unsigned long long>(ks->render_frames));
            switch_event_add_header(event, SWITCH_STACK_BOTTOM,
                "klear-cpu-seconds", "%.6f", s.cpu_seconds_total);
            switch_event_add_header(event, SWITCH_STACK_BOTTOM,
                "klear-erle-db", "%.2f", s.aec.erle_db);
            switch_event_add_header(event, SWITCH_STACK_BOTTOM,
                "klear-ns-reduction-db", "%.2f", s.ns.reduction_db);
            switch_event_fire(&event);
        }
        return SWITCH_TRUE;
    }

    case SWITCH_ABC_TYPE_WRITE_REPLACE: {
        switch_frame_t* frame = switch_core_media_bug_get_write_replace_frame(bug);
        if (!frame || !frame->data || switch_test_flag(frame, SFF_CNG)) {
            return SWITCH_TRUE;
        }
        switch_mutex_lock(ks->lock);
        if (ks->proc) {
            auto* samples = static_cast<const int16_t*>(frame->data);
            std::size_t n = frame->datalen / sizeof(int16_t);
            ks->proc->process_render(samples, n);
            ks->render_frames++;
        }
        switch_mutex_unlock(ks->lock);
        switch_core_media_bug_set_write_replace_frame(bug, frame);
        return SWITCH_TRUE;
    }

    case SWITCH_ABC_TYPE_READ_REPLACE: {
        switch_frame_t* frame = switch_core_media_bug_get_read_replace_frame(bug);
        if (!frame || !frame->data || switch_test_flag(frame, SFF_CNG)) {
            return SWITCH_TRUE;
        }
        switch_mutex_lock(ks->lock);
        if (ks->proc) {
            auto* samples = static_cast<int16_t*>(frame->data);
            std::size_t n = frame->datalen / sizeof(int16_t);
            ks->proc->process_capture(samples, n);
            ks->capture_frames++;
        }
        switch_mutex_unlock(ks->lock);
        switch_core_media_bug_set_read_replace_frame(bug, frame);
        return SWITCH_TRUE;
    }

    default:
        break;
    }
    return SWITCH_TRUE;
}

// ---- start / stop -------------------------------------------------------

switch_status_t klear_start(switch_core_session_t* session) {
    switch_channel_t* channel = switch_core_session_get_channel(session);

    if (switch_channel_get_private(channel, kChannelPrivateKey)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
            "klear: already attached to this channel\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_codec_t* read_codec = switch_core_session_get_read_codec(session);
    if (!read_codec || !read_codec->implementation) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "klear: no read codec\n");
        return SWITCH_STATUS_FALSE;
    }
    const int rate =
        read_codec->implementation->actual_samples_per_second;

    // Resolve the effective preset: channel variable wins over global default.
    Preset effective_preset = g_globals.default_preset;
    const char* preset_var = switch_channel_get_variable(channel, "klear_preset");
    if (preset_var && *preset_var) {
        if (!strcasecmp(preset_var, "agent"))          effective_preset = Preset::Agent;
        else if (!strcasecmp(preset_var, "telephony")) effective_preset = Preset::Telephony;
        else if (!strcasecmp(preset_var, "aec_only") ||
                 !strcasecmp(preset_var, "aec-only"))  effective_preset = Preset::AecOnly;
        else switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_WARNING, "klear: unknown preset '%s', using default\n",
            preset_var);
    }

    bool preset_aec = false, preset_ns = false;
    switch (effective_preset) {
        case Preset::Agent:     preset_aec = false; preset_ns = true;  break;
        case Preset::Telephony: preset_aec = true;  preset_ns = true;  break;
        case Preset::AecOnly:   preset_aec = true;  preset_ns = false; break;
    }

    // Fine-grained overrides: any explicit klear_aec / klear_ns / klear_hpf /
    // klear_ns_atten wins over the preset default.
    const bool want_aec = var_bool(channel, "klear_aec", preset_aec);
    const bool want_ns  = var_bool(channel, "klear_ns",  preset_ns);
    const bool want_hpf = var_bool(channel, "klear_hpf", g_globals.default_hpf);
    const float atten_db = var_float(channel, "klear_ns_atten",
                                     g_globals.default_ns_atten_db);

    auto* ks = new (std::nothrow) KlearSession();
    if (!ks) return SWITCH_STATUS_MEMERR;
    ks->fs_session = session;
    ks->channel_rate = rate;
    switch_mutex_init(&ks->lock, SWITCH_MUTEX_NESTED,
                      switch_core_session_get_pool(session));

    klear::ProcessorConfig pcfg;
    pcfg.sample_rate = rate;
    pcfg.aec_enabled = want_aec;
    pcfg.ns_enabled = want_ns;

    klear::WebrtcAec::Options aec_opts;
    aec_opts.high_pass_filter = want_hpf;
    auto aec_backend = std::make_unique<klear::WebrtcAec>(aec_opts);

    std::unique_ptr<klear::INsBackend> ns_backend;
    if (want_ns) {
        klear::DeepFilterNs::Options df_opts;
        df_opts.atten_lim_db = atten_db;
        df_opts.post_filter_beta = 0.0f;
        ns_backend = std::make_unique<klear::DeepFilterNs>(df_opts);
    } else {
        ns_backend = std::make_unique<klear::NullNs>();
    }

    ks->proc = std::make_unique<klear::Processor>();
    std::string err;
    if (!ks->proc->init(pcfg, std::move(aec_backend), std::move(ns_backend),
                        &err)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "klear: processor init failed: %s\n", err.c_str());
        destroy_klear(ks);
        return SWITCH_STATUS_FALSE;
    }

    switch_media_bug_t* bug = nullptr;
    const switch_media_bug_flag_t flags = static_cast<switch_media_bug_flag_t>(
        SMBF_READ_REPLACE | SMBF_WRITE_REPLACE | SMBF_NO_PAUSE);
    switch_status_t st =
        switch_core_media_bug_add(session, kBugName, nullptr, klear_callback,
                                  ks, 0, flags, &bug);
    if (st != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "klear: media_bug_add failed\n");
        destroy_klear(ks);
        return st;
    }

    switch_channel_set_private(channel, kChannelPrivateKey, bug);
    switch_channel_set_private(channel, "_klear_data_", ks);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
        "klear: attached (rate=%d aec=%d ns=%d hpf=%d atten=%.1fdB)\n",
        rate, (int)want_aec,
        (int)(want_ns && !ks->ns_disabled_rate),
        (int)want_hpf, atten_db);
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t klear_stop(switch_core_session_t* session) {
    switch_channel_t* channel = switch_core_session_get_channel(session);
    auto* bug = static_cast<switch_media_bug_t*>(
        switch_channel_get_private(channel, kChannelPrivateKey));
    if (!bug) return SWITCH_STATUS_FALSE;

    switch_channel_set_private(channel, kChannelPrivateKey, nullptr);
    auto* ks = static_cast<KlearSession*>(
        switch_channel_get_private(channel, "_klear_data_"));
    switch_channel_set_private(channel, "_klear_data_", nullptr);

    switch_core_media_bug_remove(session, &bug);
    destroy_klear(ks);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
        "klear: detached\n");
    return SWITCH_STATUS_SUCCESS;
}

// ---- hot toggles on an attached channel ---------------------------------

void apply_runtime(switch_core_session_t* session, const char* kv) {
    switch_channel_t* channel = switch_core_session_get_channel(session);
    auto* ks = static_cast<KlearSession*>(
        switch_channel_get_private(channel, "_klear_data_"));
    if (!ks || !ks->proc) return;

    // kv is a space-separated list like "aec=on ns=off"
    char buf[256];
    strncpy(buf, kv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* saveptr = nullptr;
    for (char* tok = strtok_r(buf, " \t,", &saveptr); tok;
         tok = strtok_r(nullptr, " \t,", &saveptr)) {
        char* eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = tok;
        const char* val = eq + 1;
        const bool on = (strcasecmp(val, "on") == 0 ||
                         strcasecmp(val, "true") == 0 ||
                         strcmp(val, "1") == 0);
        if (strcasecmp(key, "aec") == 0) {
            ks->proc->set_aec_enabled(on);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_INFO, "klear: aec=%d\n", (int)on);
        } else if (strcasecmp(key, "ns") == 0) {
            if (on && ks->ns_disabled_rate) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_WARNING,
                    "klear: refusing ns=on at channel rate %d\n",
                    ks->channel_rate);
            } else {
                ks->proc->set_ns_enabled(on);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_INFO, "klear: ns=%d\n", (int)on);
            }
        }
    }
}

// ---- Dialplan app -------------------------------------------------------

SWITCH_STANDARD_APP(klear_app_function) {
    const char* arg = data && *data ? data : "start";
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Split verb and remaining args
    char* verb = buf;
    char* rest = strchr(buf, ' ');
    if (rest) { *rest++ = '\0'; while (*rest == ' ') ++rest; }

    if (strcasecmp(verb, "start") == 0) {
        klear_start(session);
    } else if (strcasecmp(verb, "stop") == 0) {
        klear_stop(session);
    } else if (strcasecmp(verb, "set") == 0) {
        apply_runtime(session, rest ? rest : "");
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
            "klear: unknown app verb '%s' (use start|stop|set)\n", verb);
    }
}

// ---- API command (fs_cli / ESL) -----------------------------------------

SWITCH_STANDARD_API(klear_api_function) {
    if (!cmd || !*cmd) {
        stream->write_function(stream, "-ERR usage: klear <uuid> start|stop|set k=v ...\n");
        return SWITCH_STATUS_SUCCESS;
    }

    char buf[512];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* saveptr = nullptr;
    char* uuid = strtok_r(buf, " ", &saveptr);
    char* verb = uuid ? strtok_r(nullptr, " ", &saveptr) : nullptr;
    char* rest = saveptr;  // remainder of the string

    if (!uuid || !verb) {
        stream->write_function(stream, "-ERR usage: klear <uuid> start|stop|set k=v ...\n");
        return SWITCH_STATUS_SUCCESS;
    }

    switch_core_session_t* s = switch_core_session_locate(uuid);
    if (!s) {
        stream->write_function(stream, "-ERR no such session %s\n", uuid);
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t st = SWITCH_STATUS_SUCCESS;
    if (strcasecmp(verb, "start") == 0) {
        st = klear_start(s);
    } else if (strcasecmp(verb, "stop") == 0) {
        st = klear_stop(s);
    } else if (strcasecmp(verb, "set") == 0) {
        apply_runtime(s, rest ? rest : "");
    } else {
        stream->write_function(stream, "-ERR unknown verb %s\n", verb);
    }

    switch_core_session_rwunlock(s);
    stream->write_function(stream, "%s\n",
        st == SWITCH_STATUS_SUCCESS ? "+OK" : "-ERR");
    return SWITCH_STATUS_SUCCESS;
}

// ---- Config file --------------------------------------------------------

void load_config() {
    switch_xml_t cfg, xml, settings, param;
    if (!(xml = switch_xml_open_cfg("klear.conf", &cfg, nullptr))) {
        return;  // use compiled-in defaults
    }
    if ((settings = switch_xml_child(cfg, "settings"))) {
        for (param = switch_xml_child(settings, "param"); param;
             param = param->next) {
            const char* name = switch_xml_attr_soft(param, "name");
            const char* val = switch_xml_attr_soft(param, "value");
            if (!strcasecmp(name, "default-preset")) {
                if (!strcasecmp(val, "agent"))          g_globals.default_preset = Preset::Agent;
                else if (!strcasecmp(val, "telephony")) g_globals.default_preset = Preset::Telephony;
                else if (!strcasecmp(val, "aec_only") ||
                         !strcasecmp(val, "aec-only"))  g_globals.default_preset = Preset::AecOnly;
            }
            else if (!strcasecmp(name, "default-hpf"))
                g_globals.default_hpf = switch_true(val);
            else if (!strcasecmp(name, "default-ns-atten-db"))
                g_globals.default_ns_atten_db = static_cast<float>(atof(val));
        }
    }
    switch_xml_free(xml);
}

}  // namespace

// ---- Module load / shutdown ---------------------------------------------

SWITCH_MODULE_LOAD_FUNCTION(mod_klear_load) {
    switch_application_interface_t* app_interface = nullptr;
    switch_api_interface_t* api_interface = nullptr;

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    load_config();

    SWITCH_ADD_APP(app_interface, "klear",
        "Clean audio (AEC3 + DeepFilterNet)",
        "Attach mod_klear echo cancellation and noise suppression to this "
        "channel. Usage: klear [start|stop|set aec=on|off ns=on|off]",
        klear_app_function, "[start|stop|set key=value]", SAF_NONE);

    SWITCH_ADD_API(api_interface, "klear", "mod_klear control",
        klear_api_function,
        "klear <uuid> start|stop|set key=value ...");

    const char* preset_name = "agent";
    switch (g_globals.default_preset) {
        case Preset::Agent:     preset_name = "agent"; break;
        case Preset::Telephony: preset_name = "telephony"; break;
        case Preset::AecOnly:   preset_name = "aec_only"; break;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "mod_klear loaded (default preset=%s hpf=%d atten=%.1fdB)\n",
        preset_name, (int)g_globals.default_hpf,
        g_globals.default_ns_atten_db);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_klear_shutdown) {
    return SWITCH_STATUS_UNLOAD;
}
