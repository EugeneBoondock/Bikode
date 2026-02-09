#pragma once
/******************************************************************************
*
* Biko
*
* AIProvider.h
*   AI provider registry â€” definitions for all supported LLM backends.
*   Shared between the editor (biko.exe) and engine (biko-engine.exe).
*
*   Supported providers:
*     Cloud:  OpenAI, Anthropic, Google Gemini, Mistral, Cohere,
*             DeepSeek, xAI (Grok), Groq, OpenRouter, Together AI,
*             Fireworks AI, Perplexity
*     Local:  Ollama, LM Studio, llama.cpp server, vLLM, LocalAI
*     Custom: Any OpenAI-compatible endpoint (BYOK)
*
******************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Provider IDs
//=============================================================================

typedef enum
{
    // Cloud providers
    AI_PROVIDER_OPENAI = 0,
    AI_PROVIDER_ANTHROPIC,
    AI_PROVIDER_GOOGLE,
    AI_PROVIDER_MISTRAL,
    AI_PROVIDER_COHERE,
    AI_PROVIDER_DEEPSEEK,
    AI_PROVIDER_XAI,
    AI_PROVIDER_GROQ,
    AI_PROVIDER_OPENROUTER,
    AI_PROVIDER_TOGETHER,
    AI_PROVIDER_FIREWORKS,
    AI_PROVIDER_PERPLEXITY,

    // Local providers
    AI_PROVIDER_OLLAMA,
    AI_PROVIDER_LMSTUDIO,
    AI_PROVIDER_LLAMACPP,
    AI_PROVIDER_VLLM,
    AI_PROVIDER_LOCALAI,

    // Custom / BYOK
    AI_PROVIDER_CUSTOM,

    AI_PROVIDER_COUNT
} EAIProvider;

//=============================================================================
// Authentication method
//=============================================================================

typedef enum
{
    AI_AUTH_NONE = 0,           // No auth (local models)
    AI_AUTH_BEARER,             // Authorization: Bearer <key>
    AI_AUTH_XAPIKEY,            // x-api-key: <key>  (Anthropic)
    AI_AUTH_QUERY_PARAM,        // ?key=<key>  (Google)
    AI_AUTH_CUSTOM_HEADER       // Custom header name
} EAIAuthMethod;

//=============================================================================
// Request format (different API schemas)
//=============================================================================

typedef enum
{
    AI_FORMAT_OPENAI = 0,       // OpenAI chat completions format (most providers)
    AI_FORMAT_ANTHROPIC,        // Anthropic Messages API
    AI_FORMAT_GOOGLE,           // Google Gemini API
    AI_FORMAT_COHERE            // Cohere Chat API
} EAIRequestFormat;

//=============================================================================
// Provider definition â€” static metadata for each known provider
//=============================================================================

typedef struct
{
    EAIProvider     id;
    const char*     szName;             // Display name
    const char*     szSlug;             // Config key (lowercase, no spaces)

    // Connection
    const char*     szDefaultHost;      // Default API hostname
    const char*     szDefaultPath;      // Default API path
    int             iDefaultPort;       // Default port (443 for HTTPS, 80/11434 etc for local)
    int             bUseSSL;            // Use HTTPS

    // Auth
    EAIAuthMethod   eAuth;
    const char*     szAuthHeaderName;   // Header name if AI_AUTH_CUSTOM_HEADER
    const char*     szExtraHeaders;     // Additional required headers (semicolon-separated "Name: Value" pairs)
    const char*     szEnvVarKey;        // Environment variable name for API key

    // API format
    EAIRequestFormat eFormat;

    // Models
    const char*     szDefaultModel;
    const char*     szModels;           // Semicolon-separated list of popular models

    // Flags
    int             bRequiresKey;       // API key mandatory
    int             bIsLocal;           // Local model server
    int             bSupportsStreaming;  // Supports SSE streaming
} AIProviderDef;

//=============================================================================
// Provider registry â€” populated in AIProvider.c
//=============================================================================

// Returns the full provider registry array. Count = AI_PROVIDER_COUNT.
const AIProviderDef* AIProvider_GetRegistry(void);

// Look up a provider by ID.
const AIProviderDef* AIProvider_Get(EAIProvider id);

// Look up a provider by slug string (e.g. "openai", "ollama").
const AIProviderDef* AIProvider_FindBySlug(const char* szSlug);

// Look up a provider by display name.
const AIProviderDef* AIProvider_FindByName(const char* szName);

// Get provider count.
int AIProvider_GetCount(void);

//=============================================================================
// Active provider configuration (runtime, user-configurable)
//=============================================================================

typedef struct
{
    EAIProvider eProvider;           // Which provider

    // Connection overrides (empty = use provider defaults)
    char        szHost[512];
    char        szPath[256];
    int         iPort;
    int         bUseSSL;

    // Auth
    char        szApiKey[512];      // BYOK â€” user's own API key

    // Model
    char        szModel[128];

    // Parameters
    double      dTemperature;
    int         iMaxTokens;
    double      dTopP;
    double      dFrequencyPenalty;
    double      dPresencePenalty;

    // Behavior
    int         bStream;            // Use streaming responses
    int         iTimeoutSec;        // Request timeout in seconds
    int         iMaxRetries;        // Retry on failure
} AIProviderConfig;

// Initialize a provider config with defaults for a given provider.
void AIProviderConfig_InitDefaults(AIProviderConfig* pCfg, EAIProvider eProvider);

// Resolve the effective host/path/port/ssl for a config (fills in defaults if empty).
void AIProviderConfig_Resolve(const AIProviderConfig* pCfg,
                              const char** ppHost, const char** ppPath,
                              int* piPort, int* pbSSL);

// Serialize provider config to a JSON object string (for pipe messages).
// Caller must free returned string.
char* AIProviderConfig_ToJSON(const AIProviderConfig* pCfg);

// Parse provider config from a JSON string.
void AIProviderConfig_FromJSON(const char* szJSON, AIProviderConfig* pCfg);

//=============================================================================
// Chat Attachments
//=============================================================================

#define AI_MAX_CHAT_ATTACHMENTS 8
#define AI_ATTACHMENT_PATH_MAX   1024
#define AI_ATTACHMENT_NAME_MAX    256
#define AI_ATTACHMENT_TYPE_MAX     64

typedef struct TAIChatAttachment
{
    char path[AI_ATTACHMENT_PATH_MAX];        // Absolute path to the saved attachment (UTF-8)
    char displayName[AI_ATTACHMENT_NAME_MAX]; // Short name to show in manifest (UTF-8)
    char contentType[AI_ATTACHMENT_TYPE_MAX]; // MIME-like hint (e.g., "image/png")
    int  isImage;               // TRUE if attachment is an image/thumbnail
} AIChatAttachment;

#ifdef __cplusplus
}
#endif
