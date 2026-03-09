#include "Utils.h"
#include "Scintilla.h"
#include "SciCall.h"

// Define max words and buffer size for completion
#define BIKO_MAX_AUTOCOMPLETION_WORDS 1000
#define BIKO_MAX_WORD_LENGTH 64
#define BIKO_MAX_SCAN_BYTES (2 * 1024 * 1024)

typedef struct {
    char word[BIKO_MAX_WORD_LENGTH];
} BikoCompletionWord;

static int BikoCompareWords(const void* a, const void* b) {
    return _stricmp(((BikoCompletionWord*)a)->word, ((BikoCompletionWord*)b)->word);
}

void n2e_ShowAutoComplete(HWND hwndEdit) {
    int iCurPos = SendMessage(hwndEdit, SCI_GETCURRENTPOS, 0, 0);
    int iWordStart = SendMessage(hwndEdit, SCI_WORDSTARTPOSITION, iCurPos, TRUE);
    int iWordLen = iCurPos - iWordStart;

    if (iWordLen < 3 || iWordLen >= BIKO_MAX_WORD_LENGTH) // Only trigger if they typed at least 3 chars
        return;

    char szCurrentWord[BIKO_MAX_WORD_LENGTH] = {0};
    struct Sci_TextRange trWord;
    trWord.chrg.cpMin = iWordStart;
    trWord.chrg.cpMax = iCurPos;
    trWord.lpstrText = szCurrentWord;
    SendMessage(hwndEdit, SCI_GETTEXTRANGE, 0, (LPARAM)&trWord);

    BikoCompletionWord* words = (BikoCompletionWord*)malloc(sizeof(BikoCompletionWord) * BIKO_MAX_AUTOCOMPLETION_WORDS);
    int wordCount = 0;
    if (!words) {
        return;
    }

    // Collect words from the document
    int iDocLen = SendMessage(hwndEdit, SCI_GETLENGTH, 0, 0);
    if (iDocLen <= 0 || iDocLen > BIKO_MAX_SCAN_BYTES) {
        free(words);
        return;
    }
    char* docText = (char*)malloc(iDocLen + 1);
    if (!docText) { free(words); return; }

    struct Sci_TextRange tr;
    tr.chrg.cpMin = 0;
    tr.chrg.cpMax = iDocLen;
    tr.lpstrText = docText;
    SendMessage(hwndEdit, SCI_GETTEXTRANGE, 0, (LPARAM)&tr);
    docText[iDocLen] = '\0';

    char* context = NULL;
    char* token = strtok_s(docText, " \t\r\n.,;:!?()[]{}<>\"'=+-*/\\|%&^~$", &context);
    while (token && wordCount < BIKO_MAX_AUTOCOMPLETION_WORDS) {
        int len = strlen(token);
        if (len >= 3 && len < BIKO_MAX_WORD_LENGTH) {
            // Check if it's not the word we are currently typing (or at least different offset)
            // For simplicity, just add everything and deduplicate later
            strncpy_s(words[wordCount].word, BIKO_MAX_WORD_LENGTH, token, _TRUNCATE);
            wordCount++;
        }
        token = strtok_s(NULL, " \t\r\n.,;:!?()[]{}<>\"'=+-*/\\|%&^~$", &context);
    }

    free(docText);

    if (wordCount == 0) {
        free(words);
        return;
    }

    // Sort and deduplicate
    qsort(words, wordCount, sizeof(BikoCompletionWord), BikoCompareWords);
    
    int uniqueCount = 0;
    for (int i = 0; i < wordCount; i++) {
        if (i == 0 || _stricmp(words[i].word, words[uniqueCount - 1].word) != 0) {
            if (i != uniqueCount) {
                 strcpy_s(words[uniqueCount].word, BIKO_MAX_WORD_LENGTH, words[i].word);
            }
            uniqueCount++;
        }
    }

    // Filter by current prefix
    int matchCount = 0;
    // We will build a space separated list of words
    int listBufferSize = uniqueCount * (BIKO_MAX_WORD_LENGTH + 1);
    char* listBuffer = (char*)malloc(listBufferSize);
    if (!listBuffer) {
        free(words);
        return;
    }
    listBuffer[0] = '\0';

    for(int i=0; i<uniqueCount; i++) {
        // Does it start with our prefix? (case insensitive)
        if (_strnicmp(words[i].word, szCurrentWord, iWordLen) == 0 && _stricmp(words[i].word, szCurrentWord) != 0) {
             if (matchCount > 0)
                 strcat_s(listBuffer, listBufferSize, " ");
             strcat_s(listBuffer, listBufferSize, words[i].word);
             matchCount++;
        }
    }

    free(words);

    if (matchCount > 0) {
        SendMessage(hwndEdit, SCI_AUTOCSETIGNORECASE, TRUE, 0);
        SendMessage(hwndEdit, SCI_AUTOCSETSEPARATOR, ' ', 0);
        SendMessage(hwndEdit, SCI_AUTOCSHOW, iWordLen, (LPARAM)listBuffer);
    }

    free(listBuffer);
}
