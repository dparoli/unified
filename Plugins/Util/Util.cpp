#include "Util.hpp"

#include "API/Constants.hpp"
#include "API/Globals.hpp"
#include "API/C2DA.hpp"
#include "API/CNWRules.hpp"
#include "API/CTwoDimArrays.hpp"
#include "API/CResRef.hpp"
#include "API/CExoResMan.hpp"
#include "API/CExoString.hpp"
#include "API/CExoStringList.hpp"
#include "API/CVirtualMachine.hpp"
#include "API/CTlkTable.hpp"
#include "API/CTlkTableTokenCustom.hpp"
#include "API/CAppManager.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CWorldTimer.hpp"
#include "API/CGameObjectArray.hpp"
#include "API/Functions.hpp"
#include "Utils.hpp"
#include "ViewPtr.hpp"

#include <string>
#include <stdio.h>
#include <regex>
#include <functional>

using namespace NWNXLib;
using namespace NWNXLib::API;

static ViewPtr<Util::Util> g_plugin;

NWNX_PLUGIN_ENTRY Plugin::Info* PluginInfo()
{
    return new Plugin::Info
    {
        "Util",
        "Miscellaneous utility functions",
        "sherincall",
        "sherincall@gmail.com",
        1,
        true
    };
}

NWNX_PLUGIN_ENTRY Plugin* PluginLoad(Plugin::CreateParams params)
{
    g_plugin = new Util::Util(params);
    return g_plugin;
}


namespace Util {

Util::Util(const Plugin::CreateParams& params)
    : Plugin(params)
{
#define REGISTER(func) \
    GetServices()->m_events->RegisterEvent(#func, std::bind(&Util::func, this, std::placeholders::_1))

    REGISTER(GetCurrentScriptName);
    REGISTER(GetAsciiTableString);
    REGISTER(Hash);
    REGISTER(GetCustomToken);
    REGISTER(EffectTypeCast);
    REGISTER(GenerateUUID);
    REGISTER(StripColors);
    REGISTER(IsValidResRef);
    REGISTER(GetEnvironmentVariable);
    REGISTER(GetMinutesPerHour);
    REGISTER(SetMinutesPerHour);
    REGISTER(EncodeStringForURL);
    REGISTER(Get2DARowCount);
    REGISTER(GetFirstResRef);
    REGISTER(GetNextResRef);
    REGISTER(GetServerTicksPerSecond);
    REGISTER(GetLastCreatedObject);

#undef REGISTER

    GetServices()->m_hooks->RequestSharedHook<API::Functions::CServerExoAppInternal__MainLoop, int32_t>(
            +[](Services::Hooks::CallType type, CServerExoAppInternal*)
            {
                static int ticks;
                static time_t previous;

                if (type == Services::Hooks::CallType::AFTER_ORIGINAL)
                {
                    time_t current = time(nullptr);

                    if (current == previous)
                    {
                        ticks++;
                    }
                    else
                    {
                        g_plugin->m_tickCount = ticks;
                        previous = current;
                        ticks = 1;
                    }
                }
            });
}

Util::~Util()
{
}

ArgumentStack Util::GetCurrentScriptName(ArgumentStack&& args)
{
    ArgumentStack stack;
    std::string retval = "";

    const auto depth = Services::Events::ExtractArgument<int32_t>(args);

    auto *pVM = API::Globals::VirtualMachine();
    if (pVM && pVM->m_pVirtualMachineScript && pVM->m_nRecursionLevel >= 0 && pVM->m_nRecursionLevel >= depth)
    {
        auto& script = pVM->m_pVirtualMachineScript[pVM->m_nRecursionLevel - depth];
        if (!script.m_sScriptName.IsEmpty())
            retval = script.m_sScriptName.CStr();
    }

    Services::Events::InsertArgument(stack, retval);
    return stack;
}

ArgumentStack Util::GetAsciiTableString(ArgumentStack&& args)
{
    (void)args;
    ArgumentStack stack;
    static char table[256];

    if (table[0] == 0)
    {
        table[0] = 1; // Can't pass NULL or it will terminate the string
        for (size_t i = 1; i < sizeof(table); i++)
            table[i] = i;
    }

    Services::Events::InsertArgument(stack, std::string(table));
    return stack;
}

ArgumentStack Util::Hash(ArgumentStack&& args)
{
    ArgumentStack stack;
    const auto str = Services::Events::ExtractArgument<std::string>(args);

    Services::Events::InsertArgument(stack, (int32_t)std::hash<std::string>{}(str));
    return stack;
}

ArgumentStack Util::GetCustomToken(ArgumentStack&& args)
{
    ArgumentStack stack;
    std::string retVal;

    const auto tokenNumber = Services::Events::ExtractArgument<int32_t>(args);
      ASSERT_OR_THROW(tokenNumber >= 0);

    auto *pTlk = API::Globals::TlkTable();
    auto *pTokens = pTlk->m_pTokensCustom;
    int numTokens = pTlk->m_nTokensCustom;

    CTlkTableTokenCustom token;
    token.m_nNumber = tokenNumber;

    auto *foundToken = (CTlkTableTokenCustom*)std::bsearch(&token, pTokens, numTokens, sizeof(token),
        +[](const void *a, const void *b){ return (int32_t)((CTlkTableTokenCustom*)a)->m_nNumber - (int32_t)((CTlkTableTokenCustom*)b)->m_nNumber; });

    if(foundToken)
    {
        retVal = foundToken->m_sValue.CStr();
    }

    Services::Events::InsertArgument(stack, retVal);

    return stack;
}

ArgumentStack Util::EffectTypeCast(ArgumentStack&& args)
{
    ArgumentStack stack;
    Services::Events::InsertArgument(stack, Services::Events::ExtractArgument<CGameEffect*>(args));
    return stack;
}

ArgumentStack Util::GenerateUUID(ArgumentStack&&)
{
    ArgumentStack stack;
    uint8_t bytes[16];
    char uuid[38];

    FILE *urandom = fopen("/dev/urandom", "rb");
    ASSERT_OR_THROW(urandom);
    ASSERT(fread(bytes, 1, 16, urandom) == 16);
    fclose(urandom);

    bytes[6] = 0x40 | (bytes[6] & 0x0F);
    bytes[8] = 0x80 | (bytes[6] & 0x3F);

    snprintf(uuid, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);

    Services::Events::InsertArgument(stack, uuid);
    return stack;
}

ArgumentStack Util::StripColors(ArgumentStack&& args)
{
    ArgumentStack stack;
    const auto s = Services::Events::ExtractArgument<std::string>(args);

    std::regex color_codes("<c.+?(?=>)>|<\\/c>");
    std::string retVal = std::regex_replace(s, color_codes, "");
    Services::Events::InsertArgument(stack, retVal);
    return stack;
}

ArgumentStack Util::IsValidResRef(ArgumentStack&& args)
{
    ArgumentStack stack;
    int32_t retVal = 0;
    const auto resRef = Services::Events::ExtractArgument<std::string>(args);
    const auto resType = Services::Events::ExtractArgument<int32_t>(args);

    retVal = Globals::ExoResMan()->Exists(CResRef(resRef.c_str()), resType, nullptr);
    Services::Events::InsertArgument(stack, retVal);
    return stack;
}

ArgumentStack Util::GetEnvironmentVariable(ArgumentStack&& args)
{
    ArgumentStack stack;
    Services::Events::InsertArgument(stack, std::getenv(Services::Events::ExtractArgument<std::string>(args).c_str()));
    return stack;
}

ArgumentStack Util::GetMinutesPerHour(ArgumentStack&&)
{
    ArgumentStack stack;

    Services::Events::InsertArgument(stack,Globals::AppManager()->m_pServerExoApp->GetWorldTimer()->m_nMinutesPerHour);
    return stack;
}

ArgumentStack Util::SetMinutesPerHour(ArgumentStack&& args)
{
    ArgumentStack stack;
    const auto minPerHour = Services::Events::ExtractArgument<int32_t>(args);
      ASSERT_OR_THROW(minPerHour > 0);
      ASSERT_OR_THROW(minPerHour <= 255);

    Globals::AppManager()->m_pServerExoApp->GetWorldTimer()->SetMinutesPerHour(minPerHour);
    return stack;
}

ArgumentStack Util::EncodeStringForURL(ArgumentStack&& args)
{
    ArgumentStack stack;
    const auto s = Services::Events::ExtractArgument<std::string>(args);
    std::string result;

    // ** Copied from ../Webhook/External/httplib.h
    for (auto i = 0; s[i]; i++)
    {
        switch (s[i])
        {
            case ' ':  result += "+"; break;
            case '\'': result += "%27"; break;
            case ',':  result += "%2C"; break;
            case ':':  result += "%3A"; break;
            case ';':  result += "%3B"; break;
            default:
                if (s[i] < 0)
                {
                    result += '%';
                    char hex[4];
                    size_t len = snprintf(hex, sizeof(hex) - 1, "%02X", (unsigned char)s[i]);
                      ASSERT_OR_THROW(len == 2);
                    result.append(hex, len);
                }
                else
                {
                    result += s[i];
                }
                break;
        }
    }
    // **

    Services::Events::InsertArgument(stack, result);
    return stack;
}

ArgumentStack Util::Get2DARowCount(ArgumentStack&& args)
{
    ArgumentStack stack;
    const auto twodaRef = Services::Events::ExtractArgument<std::string>(args);
    auto twoda = Globals::Rules()->m_p2DArrays->GetCached2DA(twodaRef.c_str(), true);
    Services::Events::InsertArgument(stack, twoda ? twoda->m_nNumRows : 0);
    return stack;
}

ArgumentStack Util::GetFirstResRef(ArgumentStack&& args)
{
    ArgumentStack stack;
    std::string retVal;

    const auto resRefType = Services::Events::ExtractArgument<int32_t>(args);
    const auto regexFilter = Services::Events::ExtractArgument<std::string>(args);
    const auto bModuleOnly = Services::Events::ExtractArgument<int32_t>(args);

    m_resRefIndex = 0;
    m_listResRefs.clear();
    m_listResRefs.reserve(10);

    CExoStringList *pList = Globals::ExoResMan()->GetResOfType(resRefType, !!bModuleOnly);

    if (pList)
    {
        std::regex rxg(regexFilter);

        for (int i = 0; i < pList->m_nCount; i++)
        {
            if (regexFilter.empty() || std::regex_match(pList->m_pStrings[i]->CStr(), rxg))
            {
                m_listResRefs.emplace_back(pList->m_pStrings[i]->CStr());
            }
        }
    }

    if (m_resRefIndex < m_listResRefs.size())
    {
        retVal = m_listResRefs[m_resRefIndex];
        m_resRefIndex++;
    }

    Services::Events::InsertArgument(stack, retVal);

    return stack;
}

ArgumentStack Util::GetNextResRef(ArgumentStack&&)
{
    ArgumentStack stack;
    std::string retVal;

    if (m_resRefIndex < m_listResRefs.size())
    {
        retVal = m_listResRefs[m_resRefIndex];
        m_resRefIndex++;
    }

    Services::Events::InsertArgument(stack, retVal);

    return stack;
}

ArgumentStack Util::GetServerTicksPerSecond(ArgumentStack&&)
{
    ArgumentStack stack;
    Services::Events::InsertArgument(stack, m_tickCount);
    return stack;
}

ArgumentStack Util::GetLastCreatedObject(ArgumentStack&& args)
{
    ArgumentStack stack;
    Types::ObjectID retVal = Constants::OBJECT_INVALID;

    const auto objectType = Services::Events::ExtractArgument<int32_t>(args);
    ASSERT_OR_THROW(objectType >= 0);
    const auto nthLast = Services::Events::ExtractArgument<int32_t>(args);
    ASSERT_OR_THROW(nthLast > 0);

    auto pGameObjectArray = Globals::AppManager()->m_pServerExoApp->GetObjectArray();
    int count = 1;
    CGameObject *pObject;

    for(int nObjectID = pGameObjectArray->m_nNextObjectArrayID[0] - 1; nObjectID >= 0; nObjectID--)
    {
        if(!pGameObjectArray->GetGameObject(nObjectID, &pObject))
        {
            if (pObject && (pObject->m_nObjectType == objectType || objectType == 0))
            {
                if (count == nthLast)
                {
                    retVal = pObject->m_idSelf;
                    break;
                }
                else
                {
                    count++;
                }
            }
        }
    }

    Services::Events::InsertArgument(stack, retVal);
    return stack;
}

}
