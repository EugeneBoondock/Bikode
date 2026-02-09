#!/usr/bin/env node
/**
 * Biko Web Search - Lightweight DuckDuckGo search via Playwright
 * 
 * Usage: node web-search.js "search query" [--max=5]
 * 
 * Outputs JSON with search results to stdout.
 * Designed to be fast: headless, minimal browser context, early exit.
 */

const { chromium } = require('playwright');
const https = require('https');

const MAX_RESULTS_DEFAULT = 5;
const TIMEOUT_MS = 15000;
const DEBUG = process.env.BIKO_SEARCH_DEBUG === '1';

function normalizeResult(item) {
    return {
        title: (item.title || '').trim(),
        snippet: (item.snippet || '').trim(),
        url: (item.url || '').trim()
    };
}

function decodeDuckDuckGoRedirect(url) {
    if (!url) return '';
    try {
        const raw = url.trim();
        const normalized = raw.startsWith('//') ? `https:${raw}` : raw;
        const parsed = new URL(normalized, 'https://duckduckgo.com');
        if (parsed.pathname === '/l/' || parsed.pathname === '/l') {
            const uddg = parsed.searchParams.get('uddg');
            if (uddg) return decodeURIComponent(uddg);
        }
        return parsed.href;
    } catch (_) {
        return url;
    }
}

function decodeHtmlEntities(text) {
    if (!text) return '';
    return text
        .replace(/&amp;/g, '&')
        .replace(/&quot;/g, '"')
        .replace(/&#39;/g, "'")
        .replace(/&lt;/g, '<')
        .replace(/&gt;/g, '>')
        .replace(/&#x([0-9a-fA-F]+);/g, (_, h) => String.fromCharCode(parseInt(h, 16)))
        .replace(/&#([0-9]+);/g, (_, d) => String.fromCharCode(parseInt(d, 10)));
}

function stripTags(text) {
    return decodeHtmlEntities((text || '').replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ').trim());
}

function extractFromHtmlString(html, maxResults) {
    const items = [];
    const linkRegex = /<a[^>]*class="[^"]*result__a[^"]*"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/gi;
    let linkMatch;
    while ((linkMatch = linkRegex.exec(html)) !== null) {
        const from = linkMatch.index;
        const to = Math.min(html.length, from + 2500);
        const window = html.slice(from, to);
        const snippetMatch = window.match(/<a[^>]*class="[^"]*result__snippet[^"]*"[^>]*>([\s\S]*?)<\/a>/i)
            || window.match(/<div[^>]*class="[^"]*result__snippet[^"]*"[^>]*>([\s\S]*?)<\/div>/i);
        items.push({
            title: stripTags(linkMatch[2]),
            snippet: snippetMatch ? stripTags(snippetMatch[1]) : '',
            url: decodeHtmlEntities(linkMatch[1] || '')
        });
        if (items.length >= maxResults * 2) break;
    }
    return items;
}

function extractFromLiteHtmlString(html, maxResults) {
    const items = [];
    const linkRegex = /<a[^>]*class=['"]result-link['"][^>]*href=['"]([^'"]+)['"][^>]*>([\s\S]*?)<\/a>/gi;
    let linkMatch;
    while ((linkMatch = linkRegex.exec(html)) !== null) {
        const from = linkMatch.index;
        const to = Math.min(html.length, from + 3000);
        const window = html.slice(from, to);
        const snippetMatch = window.match(/<td[^>]*class=['"]result-snippet['"][^>]*>([\s\S]*?)<\/td>/i);
        items.push({
            title: stripTags(linkMatch[2]),
            snippet: snippetMatch ? stripTags(snippetMatch[1]) : '',
            url: decodeHtmlEntities(linkMatch[1] || '')
        });
        if (items.length >= maxResults * 2) break;
    }
    return items;
}

function uniqueResults(items, maxResults) {
    const out = [];
    const seen = new Set();
    for (const item of items) {
        const r = normalizeResult(item);
        if (!r.title || !r.url) {
            continue;
        }
        if (seen.has(r.url)) {
            continue;
        }
        seen.add(r.url);
        out.push(r);
        if (out.length >= maxResults) {
            break;
        }
    }
    return out;
}

function mergeUniqueResults(base, extras, maxResults) {
    return uniqueResults([...(base || []), ...(extras || [])], maxResults);
}

async function extractFromHtmlPage(page, maxResults) {
    return await page.evaluate((max) => {
        const items = [];
        const rows = document.querySelectorAll('.result');
        rows.forEach((el) => {
            const link = el.querySelector('.result__title a') || el.querySelector('a.result__a');
            const snippetEl = el.querySelector('.result__snippet') || el.querySelector('.result__body');
            if (!link) return;
            items.push({
                title: (link.textContent || '').trim(),
                snippet: (snippetEl?.textContent || '').trim(),
                url: link.href || (link.getAttribute('href') || '')
            });
        });
        return items.slice(0, max * 2);
    }, maxResults);
}

function fetchUrl(url) {
    return new Promise((resolve, reject) => {
        const req = https.get(url, {
            headers: {
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
                'Accept-Language': 'en-US,en;q=0.9'
            }
        }, (res) => {
            // Follow redirects.
            if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
                const redirected = new URL(res.headers.location, url).toString();
                res.resume();
                resolve(fetchUrl(redirected));
                return;
            }
            let body = '';
            res.on('data', (chunk) => { body += chunk; });
            res.on('end', () => resolve(body));
        });
        req.setTimeout(TIMEOUT_MS, () => {
            req.destroy(new Error('Request timeout'));
        });
        req.on('error', reject);
    });
}

async function extractFromHtmlResponse(page, query, maxResults) {
    const htmlUrls = [
        `https://html.duckduckgo.com/html/?q=${encodeURIComponent(query)}`,
        `https://duckduckgo.com/html/?q=${encodeURIComponent(query)}`
    ];

    for (const htmlUrl of htmlUrls) {
        let html = '';
        try {
            html = await fetchUrl(htmlUrl);
        } catch (_) {
            continue;
        }
        if (!html || !html.trim()) continue;

        const challenge = /Unfortunately,\s*bots use DuckDuckGo too\./i.test(html);
        const textResults = extractFromHtmlString(html, maxResults);
        if (DEBUG) {
            console.error(`[search-debug] url=${htmlUrl} len=${html.length} challenge=${challenge} text=${textResults.length}`);
        }

        // Prefer reliable HTML parsing first.
        if (textResults.length) return textResults;
        if (challenge) continue;

        // Fallback: let Playwright parse unusual HTML variants.
        try {
            await page.setContent(html, { waitUntil: 'domcontentloaded' });
            const domResults = await extractFromHtmlPage(page, maxResults);
            if (domResults.length) return domResults;
        } catch (_) {
            // Continue to next candidate URL.
        }
    }
    return [];
}

async function extractFromLiteResponse(query, maxResults) {
    const liteUrl = `https://lite.duckduckgo.com/lite/?q=${encodeURIComponent(query)}`;
    try {
        const html = await fetchUrl(liteUrl);
        if (!html || !html.trim()) return [];
        if (/Unfortunately,\s*bots use DuckDuckGo too\./i.test(html)) return { results: [], challenged: true };
        const results = extractFromLiteHtmlString(html, maxResults);
        if (DEBUG) {
            console.error(`[search-debug] lite len=${html.length} results=${results.length}`);
        }
        return { results, challenged: false };
    } catch (_) {
        return { results: [], challenged: false };
    }
}

async function extractFromMainPage(page, maxResults) {
    return await page.evaluate((max) => {
        const selectors = [
            'a[data-testid="result-title-a"]',
            '[data-testid="result"] h2 a',
            'article h2 a',
            '.react-results--main h2 a'
        ];
        const links = [];
        for (const sel of selectors) {
            for (const a of document.querySelectorAll(sel)) {
                links.push(a);
            }
        }
        const items = links.map((a) => {
            const container = a.closest('[data-testid="result"], article, .result');
            const snippet = container
                ? (container.querySelector('[data-result="snippet"], .result__snippet, [data-testid="result-snippet"]')?.textContent || '')
                : '';
            return {
                title: (a.textContent || '').trim(),
                snippet: snippet.trim(),
                url: a.href || (a.getAttribute('href') || '')
            };
        });
        return items.slice(0, max * 2);
    }, maxResults);
}

async function extractFromInstantAnswerApi(query, maxResults) {
    const apiUrl = `https://api.duckduckgo.com/?q=${encodeURIComponent(query)}&format=json&no_redirect=1&no_html=1`;
    const data = await new Promise((resolve, reject) => {
        const req = https.get(apiUrl, (res) => {
            let body = '';
            res.on('data', (chunk) => { body += chunk; });
            res.on('end', () => {
                try {
                    resolve(JSON.parse(body));
                } catch (err) {
                    reject(err);
                }
            });
        });
        req.setTimeout(TIMEOUT_MS, () => {
            req.destroy(new Error('Instant Answer API timeout'));
        });
        req.on('error', reject);
    });

    const items = [];
    if (data.AbstractURL && data.Heading) {
        items.push({
            title: data.Heading,
            snippet: data.AbstractText || '',
            url: data.AbstractURL
        });
    }
    const topics = Array.isArray(data.RelatedTopics) ? data.RelatedTopics : [];
    for (const topic of topics) {
        if (topic && topic.FirstURL && topic.Text) {
            items.push({
                title: topic.Text.split(' - ')[0],
                snippet: topic.Text,
                url: topic.FirstURL
            });
        } else if (topic && Array.isArray(topic.Topics)) {
            for (const nested of topic.Topics) {
                if (nested && nested.FirstURL && nested.Text) {
                    items.push({
                        title: nested.Text.split(' - ')[0],
                        snippet: nested.Text,
                        url: nested.FirstURL
                    });
                }
            }
        }
        if (items.length >= maxResults * 2) {
            break;
        }
    }
    return items;
}

async function search(query, maxResults) {
    const browser = await chromium.launch({
        headless: true,
        args: ['--disable-gpu', '--no-sandbox', '--disable-dev-shm-usage']
    });

    try {
        const context = await browser.newContext({
            userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36'
        });
        const page = await context.newPage();

        let raw = [];
        let source = 'ddg_html';
        let challenged = false;
        const attemptedQueries = [query];

        // 1) Fetch DDG HTML directly, then parse via Playwright DOM APIs.
        raw = await extractFromHtmlResponse(page, query, maxResults);
        if (!raw.length) {
            // Detect if this query is being challenged on the HTML endpoint.
            try {
                const probeHtml = await fetchUrl(`https://html.duckduckgo.com/html/?q=${encodeURIComponent(query)}`);
                challenged = /Unfortunately,\s*bots use DuckDuckGo too\./i.test(probeHtml || '');
            } catch (_) {
                // Ignore probe failures.
            }
        }

        // 2) Fallback to main DuckDuckGo web UI selectors.
        if (!raw.length) {
            source = 'ddg_main';
            const mainUrl = `https://duckduckgo.com/?q=${encodeURIComponent(query)}&ia=web`;
            await page.goto(mainUrl, { timeout: TIMEOUT_MS, waitUntil: 'domcontentloaded' });
            await page.waitForTimeout(1200);
            raw = await extractFromMainPage(page, maxResults);
        }

        // 3) Fallback to DuckDuckGo Lite endpoint (often less bot-challenged).
        if (!raw.length) {
            source = 'ddg_lite';
            const lite = await extractFromLiteResponse(query, maxResults);
            raw = lite.results;
            challenged = challenged || lite.challenged;
        }

        // 4) Final fallback: DuckDuckGo Instant Answer API (still DuckDuckGo-backed).
        if (!raw.length) {
            source = 'ddg_instant_answer_api';
            try {
                raw = await extractFromInstantAnswerApi(query, maxResults);
            } catch (_) {
                raw = [];
            }
        }

        // Decode DDG redirect wrappers (/l/?uddg=...).
        if (raw.length) {
            raw = raw.map((r) => ({
                ...r,
                url: decodeDuckDuckGoRedirect(r.url)
            }));
        }

        const results = uniqueResults(raw, maxResults);
        if (!results.length) {
            // Retry with broader variants and aggregate hits in one response.
            const variants = [];
            if (!/\bofficial\b/i.test(query)) variants.push(`${query} official`);
            if (!/\bwebsite\b/i.test(query)) variants.push(`${query} website`);
            if (!/\bgithub\b/i.test(query)) variants.push(`${query} github`);
            if (!/\blinkedin\b/i.test(query)) variants.push(`${query} linkedin`);
            if (!/\bcompany\b/i.test(query)) variants.push(`${query} company`);

            let aggregated = [];
            const effectiveQueries = [];

            for (const variant of variants) {
                attemptedQueries.push(variant);
                let variantRaw = await extractFromHtmlResponse(page, variant, maxResults);
                if (!variantRaw.length) {
                    const lite = await extractFromLiteResponse(variant, maxResults);
                    variantRaw = lite.results;
                }
                if (!variantRaw.length) {
                    try {
                        const mainUrl = `https://duckduckgo.com/?q=${encodeURIComponent(variant)}&ia=web`;
                        await page.goto(mainUrl, { timeout: TIMEOUT_MS, waitUntil: 'domcontentloaded' });
                        await page.waitForTimeout(1200);
                        variantRaw = await extractFromMainPage(page, maxResults);
                    } catch (_) {
                        variantRaw = [];
                    }
                }
                if (variantRaw.length) {
                    variantRaw = variantRaw.map((r) => ({
                        ...r,
                        url: decodeDuckDuckGoRedirect(r.url)
                    }));
                }
                const variantResults = uniqueResults(variantRaw, maxResults);
                if (variantResults.length) {
                    effectiveQueries.push(variant);
                    // Keep only top 2 from each variant, then de-dup globally.
                    aggregated = mergeUniqueResults(aggregated, variantResults.slice(0, 2), maxResults);
                    if (aggregated.length >= maxResults) break;
                }
            }

            if (aggregated.length) {
                return {
                    success: true,
                    query,
                    effectiveQueries,
                    attemptedQueries,
                    source: 'ddg_variant_bundle',
                    results: aggregated
                };
            }
        }

        if (!results.length && challenged) {
            return {
                success: false,
                query,
                source: 'ddg_challenge',
                error: 'DuckDuckGo challenge blocked automated search for this query. Retry with more context words or run manual browser search.',
                attemptedQueries,
                suggestions: [
                    `${query} official`,
                    `${query} github`,
                    `${query} website`
                ],
                results: []
            };
        }
        if (!results.length) {
            return {
                success: false,
                query,
                source: 'ddg_no_results',
                error: 'DuckDuckGo returned no web results for this query in automated mode. Try adding context keywords (official, github, location, industry).',
                attemptedQueries,
                suggestions: [
                    `${query} official`,
                    `${query} github`,
                    `${query} linkedin`
                ],
                results: []
            };
        }
        return { success: true, query, source, results };
    } catch (err) {
        return { success: false, query, error: err.message, results: [] };
    } finally {
        await browser.close();
    }
}

async function main() {
    const args = process.argv.slice(2);
    if (args.length === 0) {
        console.error('Usage: node web-search.js "query" [--max=5]');
        process.exit(1);
    }

    let query = '';
    let maxResults = MAX_RESULTS_DEFAULT;

    for (const arg of args) {
        if (arg.startsWith('--max=')) {
            maxResults = parseInt(arg.slice(6), 10) || MAX_RESULTS_DEFAULT;
        } else if (!arg.startsWith('--')) {
            query = arg;
        }
    }

    if (!query) {
        console.error('Error: No search query provided');
        process.exit(1);
    }

    const result = await search(query, maxResults);
    console.log(JSON.stringify(result, null, 2));
}

main().catch(err => {
    console.error(JSON.stringify({ success: false, error: err.message }));
    process.exit(1);
});
