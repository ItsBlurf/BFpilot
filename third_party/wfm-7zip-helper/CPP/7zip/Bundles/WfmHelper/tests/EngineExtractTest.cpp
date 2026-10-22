#include "../WfmEngine.h"
#include <stdio.h>

// CLI harness for exercising registered handlers, not just stream mocks.
int main(int argc, char **argv)
{
  if (argc < 3 || argc > 4) return 2;
  char message[1024] = {};
  const int result = WfmEngineExtract(argv[1], argv[2],
      argc == 4 ? argv[3] : "", false, NULL, message, sizeof(message));
  printf("engine result=%d message=%s\n", result, message);
  return result;
}
