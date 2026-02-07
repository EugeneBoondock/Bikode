/******************************************************************************
*
* Biko
*
* AIProvider.c
*   AI provider registry implementation.
*   Static table of all supported providers with connection details,
*   auth methods, API formats, and model lists.
*
******************************************************************************/

#include "AIProvider.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

//=============================================================================
// Compile-time provider registry
//=============================================================================

static const AIProviderDef s_providers[AI_PROVIDER_COUNT] = {

    // â”€â”€â”€ Cloud Providers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    [AI_PROVIDER_OPENAI] = {
        .id             = AI_PROVIDER_OPENAI,
        .szName         = "OpenAI",
        .szSlug         = "openai",
        .szDefaultHost  = "api.openai.com",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "OPENAI_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "gpt-5.2",
        .szModels       = "gpt-5.2;gpt-5-mini;gpt-5-nano;gpt-4.1;gpt-4.1-mini;gpt-4o;gpt-4o-mini;o3;o4-mini",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_ANTHROPIC] = {
        .id             = AI_PROVIDER_ANTHROPIC,
        .szName         = "Anthropic",
        .szSlug         = "anthropic",
        .szDefaultHost  = "api.anthropic.com",
        .szDefaultPath  = "/v1/messages",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_XAPIKEY,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = "anthropic-version: 2023-06-01",
        .szEnvVarKey    = "ANTHROPIC_API_KEY",
        .eFormat        = AI_FORMAT_ANTHROPIC,
        .szDefaultModel = "claude-opus-4-6",
        .szModels       = "claude-opus-4-6;claude-sonnet-4-5;claude-haiku-4-5;claude-sonnet-4-20250514;claude-opus-4-20250514",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_GOOGLE] = {
        .id             = AI_PROVIDER_GOOGLE,
        .szName         = "Google Gemini",
        .szSlug         = "google",
        .szDefaultHost  = "generativelanguage.googleapis.com",
        .szDefaultPath  = "/v1beta/models/%s:generateContent",  // %s = model
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_QUERY_PARAM,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "GOOGLE_API_KEY",
        .eFormat        = AI_FORMAT_GOOGLE,
        .szDefaultModel = "gemini-3-pro",
        .szModels       = "gemini-3-pro;gemini-3-flash;gemini-2.5-flash;gemini-2.5-pro;gemini-2.5-flash-lite",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_MISTRAL] = {
        .id             = AI_PROVIDER_MISTRAL,
        .szName         = "Mistral AI",
        .szSlug         = "mistral",
        .szDefaultHost  = "api.mistral.ai",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "MISTRAL_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "mistral-large-latest",
        .szModels       = "mistral-large-latest;mistral-medium-latest;mistral-small-latest;codestral-latest;open-mistral-nemo;open-mixtral-8x22b",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_COHERE] = {
        .id             = AI_PROVIDER_COHERE,
        .szName         = "Cohere",
        .szSlug         = "cohere",
        .szDefaultHost  = "api.cohere.com",
        .szDefaultPath  = "/v2/chat",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "COHERE_API_KEY",
        .eFormat        = AI_FORMAT_COHERE,
        .szDefaultModel = "command-r-plus",
        .szModels       = "command-r-plus;command-r;command-light;command-nightly",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_DEEPSEEK] = {
        .id             = AI_PROVIDER_DEEPSEEK,
        .szName         = "DeepSeek",
        .szSlug         = "deepseek",
        .szDefaultHost  = "api.deepseek.com",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "DEEPSEEK_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "deepseek-chat",
        .szModels       = "deepseek-chat;deepseek-coder;deepseek-reasoner",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_XAI] = {
        .id             = AI_PROVIDER_XAI,
        .szName         = "xAI (Grok)",
        .szSlug         = "xai",
        .szDefaultHost  = "api.x.ai",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "XAI_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "grok-3",
        .szModels       = "grok-3;grok-3-mini;grok-2",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_GROQ] = {
        .id             = AI_PROVIDER_GROQ,
        .szName         = "Groq",
        .szSlug         = "groq",
        .szDefaultHost  = "api.groq.com",
        .szDefaultPath  = "/openai/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "GROQ_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "llama-3.3-70b-versatile",
        .szModels       = "llama-3.3-70b-versatile;llama-3.1-8b-instant;mixtral-8x7b-32768;gemma2-9b-it",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_OPENROUTER] = {
        .id             = AI_PROVIDER_OPENROUTER,
        .szName         = "OpenRouter",
        .szSlug         = "openrouter",
        .szDefaultHost  = "openrouter.ai",
        .szDefaultPath  = "/api/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = "HTTP-Referer: https://biko-editor.dev;X-Title: Biko",
        .szEnvVarKey    = "OPENROUTER_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "anthropic/claude-sonnet-4-20250514",
        .szModels       = "anthropic/claude-sonnet-4-20250514;openai/gpt-4o;google/gemini-2.0-flash-exp;meta-llama/llama-3.3-70b-instruct;mistralai/mistral-large-2411;deepseek/deepseek-chat",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_TOGETHER] = {
        .id             = AI_PROVIDER_TOGETHER,
        .szName         = "Together AI",
        .szSlug         = "together",
        .szDefaultHost  = "api.together.xyz",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "TOGETHER_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "meta-llama/Llama-3.3-70B-Instruct-Turbo",
        .szModels       = "meta-llama/Llama-3.3-70B-Instruct-Turbo;Qwen/Qwen2.5-Coder-32B-Instruct;mistralai/Mixtral-8x22B-Instruct-v0.1;deepseek-ai/DeepSeek-R1",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_FIREWORKS] = {
        .id             = AI_PROVIDER_FIREWORKS,
        .szName         = "Fireworks AI",
        .szSlug         = "fireworks",
        .szDefaultHost  = "api.fireworks.ai",
        .szDefaultPath  = "/inference/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "FIREWORKS_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "accounts/fireworks/models/llama-v3p3-70b-instruct",
        .szModels       = "accounts/fireworks/models/llama-v3p3-70b-instruct;accounts/fireworks/models/qwen2p5-coder-32b-instruct;accounts/fireworks/models/deepseek-r1",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_PERPLEXITY] = {
        .id             = AI_PROVIDER_PERPLEXITY,
        .szName         = "Perplexity",
        .szSlug         = "perplexity",
        .szDefaultHost  = "api.perplexity.ai",
        .szDefaultPath  = "/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "PERPLEXITY_API_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "sonar-pro",
        .szModels       = "sonar-pro;sonar;sonar-reasoning",
        .bRequiresKey   = 1,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },

    // â”€â”€â”€ Local Providers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    [AI_PROVIDER_OLLAMA] = {
        .id             = AI_PROVIDER_OLLAMA,
        .szName         = "Ollama",
        .szSlug         = "ollama",
        .szDefaultHost  = "127.0.0.1",
        .szDefaultPath  = "/api/chat",
        .iDefaultPort   = 11434,
        .bUseSSL        = 0,
        .eAuth          = AI_AUTH_NONE,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = NULL,
        .eFormat        = AI_FORMAT_OPENAI,     // Ollama uses OpenAI-compat format
        .szDefaultModel = "llama3.2",
        .szModels       = "llama3.2;llama3.1;codellama;mistral;mixtral;deepseek-coder-v2;qwen2.5-coder;phi3;gemma2;starcoder2",
        .bRequiresKey   = 0,
        .bIsLocal       = 1,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_LMSTUDIO] = {
        .id             = AI_PROVIDER_LMSTUDIO,
        .szName         = "LM Studio",
        .szSlug         = "lmstudio",
        .szDefaultHost  = "127.0.0.1",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 1234,
        .bUseSSL        = 0,
        .eAuth          = AI_AUTH_NONE,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = NULL,
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "local-model",
        .szModels       = "local-model",
        .bRequiresKey   = 0,
        .bIsLocal       = 1,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_LLAMACPP] = {
        .id             = AI_PROVIDER_LLAMACPP,
        .szName         = "llama.cpp Server",
        .szSlug         = "llamacpp",
        .szDefaultHost  = "127.0.0.1",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 8080,
        .bUseSSL        = 0,
        .eAuth          = AI_AUTH_NONE,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = NULL,
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "default",
        .szModels       = "default",
        .bRequiresKey   = 0,
        .bIsLocal       = 1,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_VLLM] = {
        .id             = AI_PROVIDER_VLLM,
        .szName         = "vLLM",
        .szSlug         = "vllm",
        .szDefaultHost  = "127.0.0.1",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 8000,
        .bUseSSL        = 0,
        .eAuth          = AI_AUTH_NONE,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = NULL,
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "default",
        .szModels       = "default",
        .bRequiresKey   = 0,
        .bIsLocal       = 1,
        .bSupportsStreaming = 1,
    },

    [AI_PROVIDER_LOCALAI] = {
        .id             = AI_PROVIDER_LOCALAI,
        .szName         = "LocalAI",
        .szSlug         = "localai",
        .szDefaultHost  = "127.0.0.1",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 8080,
        .bUseSSL        = 0,
        .eAuth          = AI_AUTH_NONE,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = NULL,
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "default",
        .szModels       = "default",
        .bRequiresKey   = 0,
        .bIsLocal       = 1,
        .bSupportsStreaming = 1,
    },

    // â”€â”€â”€ Custom / BYOK â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    [AI_PROVIDER_CUSTOM] = {
        .id             = AI_PROVIDER_CUSTOM,
        .szName         = "Custom (OpenAI-compatible)",
        .szSlug         = "custom",
        .szDefaultHost  = "127.0.0.1",
        .szDefaultPath  = "/v1/chat/completions",
        .iDefaultPort   = 443,
        .bUseSSL        = 1,
        .eAuth          = AI_AUTH_BEARER,
        .szAuthHeaderName = NULL,
        .szExtraHeaders = NULL,
        .szEnvVarKey    = "BIKO_AI_KEY",
        .eFormat        = AI_FORMAT_OPENAI,
        .szDefaultModel = "default",
        .szModels       = "default",
        .bRequiresKey   = 0,
        .bIsLocal       = 0,
        .bSupportsStreaming = 1,
    },
};

//=============================================================================
// Public: Registry accessors
//=============================================================================

const AIProviderDef* AIProvider_GetRegistry(void)
{
    return s_providers;
}

const AIProviderDef* AIProvider_Get(EAIProvider id)
{
    if (id < 0 || id >= AI_PROVIDER_COUNT) return NULL;
    return &s_providers[id];
}

const AIProviderDef* AIProvider_FindBySlug(const char* szSlug)
{
    if (!szSlug) return NULL;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++)
    {
        if (s_providers[i].szSlug && _stricmp(s_providers[i].szSlug, szSlug) == 0)
            return &s_providers[i];
    }
    return NULL;
}

const AIProviderDef* AIProvider_FindByName(const char* szName)
{
    if (!szName) return NULL;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++)
    {
        if (s_providers[i].szName && _stricmp(s_providers[i].szName, szName) == 0)
            return &s_providers[i];
    }
    return NULL;
}

int AIProvider_GetCount(void)
{
    return AI_PROVIDER_COUNT;
}

//=============================================================================
// Public: AIProviderConfig helpers
//=============================================================================

void AIProviderConfig_InitDefaults(AIProviderConfig* pCfg, EAIProvider eProvider)
{
    if (!pCfg) return;
    memset(pCfg, 0, sizeof(*pCfg));

    pCfg->eProvider = eProvider;
    pCfg->dTemperature = 0.2;
    pCfg->iMaxTokens = 4096;
    pCfg->dTopP = 1.0;
    pCfg->dFrequencyPenalty = 0.0;
    pCfg->dPresencePenalty = 0.0;
    pCfg->bStream = 0;
    pCfg->iTimeoutSec = 120;
    pCfg->iMaxRetries = 2;

    const AIProviderDef* pDef = AIProvider_Get(eProvider);
    if (pDef)
    {
        if (pDef->szDefaultModel)
            strncpy(pCfg->szModel, pDef->szDefaultModel, sizeof(pCfg->szModel) - 1);

        // Try to load API key from environment
        if (pDef->szEnvVarKey)
        {
#ifdef _WIN32
            char buf[512];
            DWORD len = GetEnvironmentVariableA(pDef->szEnvVarKey, buf, sizeof(buf));
            if (len > 0 && len < sizeof(buf))
                strncpy(pCfg->szApiKey, buf, sizeof(pCfg->szApiKey) - 1);
#else
            const char* val = getenv(pDef->szEnvVarKey);
            if (val)
                strncpy(pCfg->szApiKey, val, sizeof(pCfg->szApiKey) - 1);
#endif
        }
    }
}

void AIProviderConfig_Resolve(const AIProviderConfig* pCfg,
                              const char** ppHost, const char** ppPath,
                              int* piPort, int* pbSSL)
{
    static char s_resolvedHost[512];
    static char s_resolvedPath[256];

    const AIProviderDef* pDef = AIProvider_Get(pCfg->eProvider);

    // Host
    if (pCfg->szHost[0])
    {
        strncpy(s_resolvedHost, pCfg->szHost, sizeof(s_resolvedHost) - 1);
    }
    else if (pDef)
    {
        strncpy(s_resolvedHost, pDef->szDefaultHost, sizeof(s_resolvedHost) - 1);
    }
    else
    {
        strcpy(s_resolvedHost, "127.0.0.1");
    }
    if (ppHost) *ppHost = s_resolvedHost;

    // Path â€” special handling for Google (model in path)
    if (pCfg->szPath[0])
    {
        strncpy(s_resolvedPath, pCfg->szPath, sizeof(s_resolvedPath) - 1);
    }
    else if (pDef)
    {
        if (pDef->eFormat == AI_FORMAT_GOOGLE && strstr(pDef->szDefaultPath, "%s"))
        {
            snprintf(s_resolvedPath, sizeof(s_resolvedPath), pDef->szDefaultPath,
                     pCfg->szModel[0] ? pCfg->szModel : pDef->szDefaultModel);
        }
        else
        {
            strncpy(s_resolvedPath, pDef->szDefaultPath, sizeof(s_resolvedPath) - 1);
        }
    }
    else
    {
        strcpy(s_resolvedPath, "/v1/chat/completions");
    }
    if (ppPath) *ppPath = s_resolvedPath;

    // Port
    if (piPort)
    {
        if (pCfg->iPort > 0)
            *piPort = pCfg->iPort;
        else if (pDef)
            *piPort = pDef->iDefaultPort;
        else
            *piPort = 443;
    }

    // SSL
    if (pbSSL)
    {
        if (pCfg->iPort > 0)
            *pbSSL = pCfg->bUseSSL;
        else if (pDef)
            *pbSSL = pDef->bUseSSL;
        else
            *pbSSL = 1;
    }
}

//=============================================================================
// JSON serialization helpers (minimal, no dependency on mono_json)
//=============================================================================

// These are used by both the editor and engine, so we keep them self-contained.

static void json_esc(char* dst, int dstSize, const char* src)
{
    int di = 0;
    dst[di++] = '"';
    while (*src && di < dstSize - 3)
    {
        switch (*src)
        {
        case '"':  dst[di++] = '\\'; dst[di++] = '"'; break;
        case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
        case '\n': dst[di++] = '\\'; dst[di++] = 'n'; break;
        case '\r': dst[di++] = '\\'; dst[di++] = 'r'; break;
        case '\t': dst[di++] = '\\'; dst[di++] = 't'; break;
        default:   dst[di++] = *src; break;
        }
        src++;
    }
    dst[di++] = '"';
    dst[di] = '\0';
}

char* AIProviderConfig_ToJSON(const AIProviderConfig* pCfg)
{
    if (!pCfg) return NULL;

    const AIProviderDef* pDef = AIProvider_Get(pCfg->eProvider);

    char buf[4096];
    char escaped[1024];

    int n = snprintf(buf, sizeof(buf), "{\"provider\":");
    json_esc(escaped, sizeof(escaped), pDef ? pDef->szSlug : "custom");
    n += snprintf(buf + n, sizeof(buf) - n, "%s", escaped);

    if (pCfg->szHost[0])
    {
        json_esc(escaped, sizeof(escaped), pCfg->szHost);
        n += snprintf(buf + n, sizeof(buf) - n, ",\"host\":%s", escaped);
    }
    if (pCfg->szPath[0])
    {
        json_esc(escaped, sizeof(escaped), pCfg->szPath);
        n += snprintf(buf + n, sizeof(buf) - n, ",\"path\":%s", escaped);
    }
    if (pCfg->iPort > 0)
        n += snprintf(buf + n, sizeof(buf) - n, ",\"port\":%d", pCfg->iPort);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"ssl\":%s", pCfg->bUseSSL ? "true" : "false");

    if (pCfg->szApiKey[0])
    {
        json_esc(escaped, sizeof(escaped), pCfg->szApiKey);
        n += snprintf(buf + n, sizeof(buf) - n, ",\"apiKey\":%s", escaped);
    }
    if (pCfg->szModel[0])
    {
        json_esc(escaped, sizeof(escaped), pCfg->szModel);
        n += snprintf(buf + n, sizeof(buf) - n, ",\"model\":%s", escaped);
    }

    n += snprintf(buf + n, sizeof(buf) - n, ",\"temperature\":%.2f", pCfg->dTemperature);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"maxTokens\":%d", pCfg->iMaxTokens);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"topP\":%.2f", pCfg->dTopP);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"stream\":%s", pCfg->bStream ? "true" : "false");
    n += snprintf(buf + n, sizeof(buf) - n, ",\"timeout\":%d", pCfg->iTimeoutSec);

    n += snprintf(buf + n, sizeof(buf) - n, "}");

    char* result = (char*)malloc(n + 1);
    if (result) memcpy(result, buf, n + 1);
    return result;
}

void AIProviderConfig_FromJSON(const char* szJSON, AIProviderConfig* pCfg)
{
    if (!szJSON || !pCfg) return;

    // Very basic extraction â€” looks for known keys
    // This is intentionally simple; the engine will use the same format

    // Find provider slug
    const char* p = strstr(szJSON, "\"provider\"");
    if (p)
    {
        p = strchr(p + 10, ':');
        if (p)
        {
            p++;
            while (*p == ' ') p++;
            if (*p == '"')
            {
                p++;
                char slug[64] = {0};
                int i = 0;
                while (*p && *p != '"' && i < 63) slug[i++] = *p++;
                const AIProviderDef* pDef = AIProvider_FindBySlug(slug);
                if (pDef)
                    pCfg->eProvider = pDef->id;
            }
        }
    }

    // Extract string fields (simplified)
    #define EXTRACT_STR(key, field, maxLen) do { \
        const char* _p = strstr(szJSON, "\"" key "\""); \
        if (_p) { \
            _p = strchr(_p + strlen(key) + 2, ':'); \
            if (_p) { _p++; while (*_p == ' ') _p++; \
            if (*_p == '"') { _p++; int _i = 0; \
            while (*_p && *_p != '"' && _i < (maxLen)-1) { \
                if (*_p == '\\' && *(_p+1)) { _p++; \
                    switch(*_p) { case 'n': field[_i++]='\n'; break; case 't': field[_i++]='\t'; break; \
                    default: field[_i++]=*_p; } } \
                else field[_i++] = *_p; _p++; } field[_i]='\0'; } } } \
    } while(0)

    EXTRACT_STR("host", pCfg->szHost, sizeof(pCfg->szHost));
    EXTRACT_STR("path", pCfg->szPath, sizeof(pCfg->szPath));
    EXTRACT_STR("apiKey", pCfg->szApiKey, sizeof(pCfg->szApiKey));
    EXTRACT_STR("model", pCfg->szModel, sizeof(pCfg->szModel));

    #undef EXTRACT_STR

    // Extract numeric fields
    #define EXTRACT_INT(key, field) do { \
        const char* _p = strstr(szJSON, "\"" key "\""); \
        if (_p) { _p = strchr(_p + strlen(key) + 2, ':'); \
        if (_p) { _p++; while (*_p == ' ') _p++; field = atoi(_p); } } \
    } while(0)

    EXTRACT_INT("port", pCfg->iPort);
    EXTRACT_INT("maxTokens", pCfg->iMaxTokens);
    EXTRACT_INT("timeout", pCfg->iTimeoutSec);

    #undef EXTRACT_INT

    // Extract doubles
    const char* tp = strstr(szJSON, "\"temperature\"");
    if (tp)
    {
        tp = strchr(tp + 13, ':');
        if (tp) { tp++; while (*tp == ' ') tp++; pCfg->dTemperature = atof(tp); }
    }
    tp = strstr(szJSON, "\"topP\"");
    if (tp)
    {
        tp = strchr(tp + 5, ':');
        if (tp) { tp++; while (*tp == ' ') tp++; pCfg->dTopP = atof(tp); }
    }

    // Extract booleans
    const char* sp = strstr(szJSON, "\"ssl\"");
    if (sp)
    {
        sp = strchr(sp + 5, ':');
        if (sp) { sp++; while (*sp == ' ') sp++; pCfg->bUseSSL = (*sp == 't') ? 1 : 0; }
    }
    sp = strstr(szJSON, "\"stream\"");
    if (sp)
    {
        sp = strchr(sp + 8, ':');
        if (sp) { sp++; while (*sp == ' ') sp++; pCfg->bStream = (*sp == 't') ? 1 : 0; }
    }
}
