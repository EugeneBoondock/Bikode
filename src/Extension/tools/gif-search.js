#!/usr/bin/env node
/**
 * gif-search.js
 * Lightweight GIF finder that returns an existing, direct GIF media URL.
 *
 * Usage:
 *   node gif-search.js "funny coding gif"
 */

const https = require('https');

const TIMEOUT_MS = 12000;
const MAX_CANDIDATES = 24;

function fetchUrl(url) {
    return new Promise((resolve, reject) => {
        const req = https.get(url, {
            headers: {
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
                'Accept-Language': 'en-US,en;q=0.9'
            }
        }, (res) => {
            if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
                const next = new URL(res.headers.location, url).toString();
                res.resume();
                resolve(fetchUrl(next));
                return;
            }
            let body = '';
            res.on('data', (c) => { body += c; });
            res.on('end', () => resolve(body));
        });
        req.setTimeout(TIMEOUT_MS, () => req.destroy(new Error('timeout')));
        req.on('error', reject);
    });
}

function decodeHtmlEntities(text) {
    return (text || '')
        .replace(/&amp;/g, '&')
        .replace(/&quot;/g, '"')
        .replace(/&#39;/g, "'")
        .replace(/&lt;/g, '<')
        .replace(/&gt;/g, '>');
}

function decodeDuckRedirect(url) {
    try {
        const norm = (url || '').startsWith('//') ? `https:${url}` : (url || '');
        const u = new URL(norm, 'https://duckduckgo.com');
        if (u.pathname === '/l/' || u.pathname === '/l') {
            const uddg = u.searchParams.get('uddg');
            if (uddg) return decodeURIComponent(uddg);
        }
        return u.href;
    } catch (_) {
        return url || '';
    }
}

function looksLikeDirectGif(url) {
    const s = (url || '').toLowerCase();
    return (
        s.includes('.gif') ||
        s.includes('media.tenor.com/') ||
        s.includes('i.giphy.com/media/')
    );
}

function extractGifCandidatesFromHtml(html) {
    const out = [];

    // Direct media links present in page source.
    const directGifRegex = /https?:\/\/[^\s"'<>]+?\.gif(?:\?[^\s"'<>]*)?/gi;
    let m;
    while ((m = directGifRegex.exec(html)) !== null) {
        out.push(m[0]);
    }

    // DDG result links that may wrap URLs.
    const linkRegex = /<a[^>]*class="[^"]*result__a[^"]*"[^>]*href="([^"]+)"/gi;
    while ((m = linkRegex.exec(html)) !== null) {
        const raw = decodeHtmlEntities(m[1] || '');
        const resolved = decodeDuckRedirect(raw);
        if (looksLikeDirectGif(resolved)) out.push(resolved);
    }

    const uniq = [];
    const seen = new Set();
    for (const u of out) {
        const key = (u || '').trim();
        if (!key || seen.has(key)) continue;
        seen.add(key);
        uniq.push(key);
        if (uniq.length >= MAX_CANDIDATES) break;
    }
    return uniq;
}

function sanitizeTenorMediaUrl(url) {
    const u = (url || '').trim();
    if (!u) return '';
    if (u.toLowerCase().includes('.gif')) return u;
    if (u.includes('media.tenor.com/')) {
        if (u.endsWith('/')) return `${u}tenor.gif`;
        return `${u}/tenor.gif`;
    }
    return u;
}

function extractFromTenorSearchHtml(html) {
    const out = [];
    const re = /https?:\/\/media\.tenor\.com\/[^\s"'<>]+/gi;
    let m;
    while ((m = re.exec(html || '')) !== null) {
        const c = sanitizeTenorMediaUrl(m[0]);
        if (looksLikeDirectGif(c)) out.push(c);
    }
    return Array.from(new Set(out)).slice(0, MAX_CANDIDATES);
}

function validateGifUrl(url) {
    return new Promise((resolve) => {
        let settled = false;
        const done = (ok, reason) => {
            if (!settled) {
                settled = true;
                resolve({ ok, reason });
            }
        };

        const req = https.get(url, {
            headers: { 'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)' }
        }, (res) => {
            const ctype = (res.headers['content-type'] || '').toLowerCase();
            const statusOk = res.statusCode >= 200 && res.statusCode < 300;
            if (!statusOk) {
                res.resume();
                done(false, `http_${res.statusCode}`);
                return;
            }
            if (!ctype.includes('image/gif')) {
                res.resume();
                done(false, `type_${ctype || 'unknown'}`);
                return;
            }
            res.resume();
            done(true, 'ok');
        });

        req.setTimeout(TIMEOUT_MS, () => {
            req.destroy(new Error('timeout'));
            done(false, 'timeout');
        });
        req.on('error', () => done(false, 'network_error'));
    });
}

async function searchGif(query) {
    const enriched = `${query} reaction gif media.tenor.com`;
    const ddgUrl = `https://html.duckduckgo.com/html/?q=${encodeURIComponent(enriched)}`;
    const html = await fetchUrl(ddgUrl);
    const candidates = extractGifCandidatesFromHtml(html || '');

    for (const c of candidates) {
        const verdict = await validateGifUrl(c);
        if (verdict.ok) {
            return {
                success: true,
                query,
                url: c,
                source: 'duckduckgo+validated',
                candidatesChecked: candidates.length
            };
        }
    }

    try {
        const tenorSlug = query.trim().replace(/\s+/g, '-');
        const tenorUrl = `https://tenor.com/search/${encodeURIComponent(tenorSlug)}-gifs`;
        const tenorHtml = await fetchUrl(tenorUrl);
        const tenorCandidates = extractFromTenorSearchHtml(tenorHtml);
        for (const c of tenorCandidates) {
            const verdict = await validateGifUrl(c);
            if (verdict.ok) {
                return {
                    success: true,
                    query,
                    url: c,
                    source: 'tenor+validated',
                    candidatesChecked: candidates.length + tenorCandidates.length
                };
            }
        }
    } catch (_) {
        // continue with failure payload
    }

    return {
        success: false,
        query,
        error: 'No valid direct GIF URL found for this context',
        source: 'duckduckgo+validated',
        candidatesChecked: candidates.length,
        suggestions: [
            `${query} funny reaction gif`,
            `${query} thinking reaction gif`,
            `${query} celebration gif`
        ]
    };
}

async function main() {
    const query = process.argv.slice(2).join(' ').trim();
    if (!query) {
        console.error(JSON.stringify({ success: false, error: 'No query provided' }));
        process.exit(1);
    }

    const result = await searchGif(query);
    console.log(JSON.stringify(result, null, 2));
}

main().catch((err) => {
    console.error(JSON.stringify({ success: false, error: err.message || String(err) }));
    process.exit(1);
});
