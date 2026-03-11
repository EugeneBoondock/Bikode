$path = 'src/Extension/ChatPanel.c'
$content = [IO.File]::ReadAllText($path, [Text.Encoding]::Unicode)
if ($content -notmatch 'AISubscriptionAgent\.h') {
  $content = $content.Replace(@'
#include "AICommands.h"
#include "AIBridge.h"
#include "AIProvider.h"
'@, @'
#include "AICommands.h"
#include "AISubscriptionAgent.h"
#include "AIBridge.h"
#include "AIProvider.h"
'@)
}
[IO.File]::WriteAllText($path, $content, [Text.Encoding]::Unicode)
