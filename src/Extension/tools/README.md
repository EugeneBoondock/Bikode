# Biko AI Tools

This directory contains helper tools for the Biko AI engine.

## Web Search (DuckDuckGo via Playwright)

The `web-search.js` script provides web search capability using Playwright and DuckDuckGo.

### Setup

```bash
cd src/Extension/tools
npm install
npx playwright install chromium
```

### Usage

From command line:
```bash
node web-search.js "your search query" --max=5
```

From the AI engine, send a request:
```json
{
    "type": "websearch",
    "chatMessage": "your search query"
}
```

### Response Format

```json
{
    "type": "websearch_result",
    "query": "your search query",
    "success": true,
    "results": [
        {
            "title": "Result Title",
            "snippet": "Description text...",
            "url": "https://example.com"
        }
    ]
}
```

### Notes

- Uses DuckDuckGo HTML version for speed (no JavaScript execution needed)
- Runs headless Chromium for reliability
- 30-second timeout on the engine side
- Results capped at 5 by default (configurable with --max)
