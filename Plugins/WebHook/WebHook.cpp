#include "WebHook.hpp"
#include "API/CNWSModule.hpp"
#include "External/httplib.h"
#include "Services/Tasks/Tasks.hpp"
#include "Services/Messaging/Messaging.hpp"
#include "Encoding.hpp"
#include <cmath>
#include <sstream>
#include <iomanip>

using namespace NWNXLib;

static ViewPtr<WebHook::WebHook> g_plugin;

NWNX_PLUGIN_ENTRY Plugin::Info* PluginInfo()
{
    return new Plugin::Info
    {
        "WebHook",
        "Users can send Slack-formatted HTTP/HTTPS webhooks with this plugin.",
        "Liareth",
        "liarethnwn@gmail.com",
        1,
        true
    };
}

NWNX_PLUGIN_ENTRY Plugin* PluginLoad(Plugin::CreateParams params)
{
    g_plugin = new WebHook::WebHook(params);
    return g_plugin;
}

using namespace NWNXLib::Services;

namespace WebHook {

WebHook::WebHook(const Plugin::CreateParams& params)
    : Plugin(params)
{
#define REGISTER(func) \
    GetServices()->m_events->RegisterEvent(#func, std::bind(&WebHook::func, this, std::placeholders::_1))

    REGISTER(OnSendWebHookHTTPS);
    REGISTER(EncodeURL);

#undef REGISTER

}

WebHook::~WebHook()
{ }

std::string escape_json(const std::string &s) {
    std::ostringstream o;
    for (auto c = s.cbegin(); c != s.cend(); c++) {
        if (*c == '"' || *c == '\\' || ('\x00' <= *c && *c <= '\x1f')) {
            o << "\\u"
              << std::hex << std::setw(4) << std::setfill('0') << (int)*c;
        } else {
            o << *c;
        }
    }
    return o.str();
}

ArgumentStack WebHook::OnSendWebHookHTTPS(ArgumentStack&& args)
{
    Events::ArgumentStack stack;
    auto host = Services::Events::ExtractArgument<std::string>(args);
    auto origPath = Services::Events::ExtractArgument<std::string>(args);
    auto message = Services::Events::ExtractArgument<std::string>(args);
    auto originalMsg = message;

    // For Discord, will wait for a response
    auto path = origPath + "?wait=true";

    WebHook& plugin = *g_plugin;

    message = Encoding::ToUTF8(message);
    escape_json(message);

    plugin.GetServices()->m_tasks->QueueOnAsyncThread([originalMsg, message, host, path, origPath]()
    {
        auto messaging = g_plugin->GetServices()->m_messaging.get();
        auto moduleOid = NWNXLib::Utils::ObjectIDToString(Utils::GetModule()->m_idSelf);

        static std::unordered_map<std::string, std::unique_ptr<httplib::SSLClient>> s_ClientCache;
        auto cli = s_ClientCache.find(host);

        if (cli == std::end(s_ClientCache))
        {
            LOG_DEBUG("Creating new SSL client for host %s.", host.c_str());
            cli = s_ClientCache.insert(std::make_pair(host, std::make_unique<httplib::SSLClient>(host.c_str(), 443))).first;
        }

        auto res = cli->second->post(path.c_str(), message, "application/json");

        if (res)
        {
            messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"STATUS", std::to_string(res->status)});
            messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"MESSAGE", message});
            messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"HOST", host});
            messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"PATH", origPath});
            if (res->status == 200 || res->status == 201 || res->status == 204 || res->status == 429)
            {
                // Discord sends your rate limit information even on success so you can stagger calls if you want
                // This header also lets us know it's Discord not Slack, important because Discord sends RETRY_AFTER
                // in milliseconds and Slack sends it as seconds.
                if (!res->get_header_value("X-RateLimit-Limit").empty())
                {
                    messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"RATELIMIT_LIMIT", res->get_header_value("X-RateLimit-Limit")});
                    messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"RATELIMIT_REMAINING", res->get_header_value("X-RateLimit-Remaining")});
                    messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"RATELIMIT_RESET", res->get_header_value("X-RateLimit-Reset")});
                    if (!res->get_header_value("Retry-After").empty())
                        messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"RETRY_AFTER", res->get_header_value("Retry-After")});
                }
                // Slack rate limited
                else if (!res->get_header_value("Retry-After").empty())
                {
                    float fSlackRetry = stof(res->get_header_value("Retry-After")) * 1000.0f;
                    messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"RETRY_AFTER", std::to_string(fSlackRetry)});
                }
                if (res->status != 429)
                {
                    messaging->BroadcastMessage("NWNX_EVENT_SIGNAL_EVENT", {"NWNX_ON_WEBHOOK_SUCCESS", moduleOid});
                    LOG_INFO("Sent webhook '%s' to '%s%s'.", message.c_str(), host.c_str(), path.c_str());

                }
                else
                {
                    messaging->BroadcastMessage("NWNX_EVENT_SIGNAL_EVENT", {"NWNX_ON_WEBHOOK_FAILED", moduleOid});
                    LOG_WARNING("Failed to send WebHook (HTTPS) message '%s' to '%s%s'. Rate Limited.", message.c_str(), host.c_str(), path.c_str());
                }
            }
            else
            {
                messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"FAIL_INFO", res->body});
                messaging->BroadcastMessage("NWNX_EVENT_SIGNAL_EVENT", {"NWNX_ON_WEBHOOK_FAILED", moduleOid});
                LOG_WARNING("Failed to send WebHook (HTTPS) message '%s' to '%s%s', status code '%d'.", message.c_str(), host.c_str(), path.c_str(), res->status);
            }
        }
        else
        {
            messaging->BroadcastMessage("NWNX_EVENT_PUSH_EVENT_DATA", {"FAIL_INFO", "Failed to post to server. Is the url correct?"});
            messaging->BroadcastMessage("NWNX_EVENT_SIGNAL_EVENT", {"NWNX_ON_WEBHOOK_FAILED", moduleOid});
            LOG_WARNING("Failed to send WebHook (HTTPS) to '%s%s'.", host.c_str(), path.c_str());
        }
    });

    return stack;
}

ArgumentStack WebHook::EncodeURL(ArgumentStack&& args)
{
    Events::ArgumentStack stack;
    const auto url = Services::Events::ExtractArgument<std::string>(args);

    Services::Events::InsertArgument(stack, httplib::detail::encode_url(url));
    return stack;
}

}
