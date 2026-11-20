// Standalone stdlib-only runner, also covered by the normal unit target.
#include "DeletionJournalUnitTests.h"
int main(int argc, char **argv) {
    usapptest::deletionjournalunit::runDeletionJournalUnitTests();
    if (argc == 2) usapptest::deletionjournalunit::exportScenario(argv[1]);
    else if (argc != 1) return 2;
    return usapptest::finish();
}
