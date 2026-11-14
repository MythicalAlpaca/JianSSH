#pragma once
#include <string>
namespace jianssh::ui {
struct KeyFilePickerResult { std::string path, error; };
// Blocking native dialog; invoke only on a worker thread. Empty fields mean cancellation.
KeyFilePickerResult choose_key_file();
}
