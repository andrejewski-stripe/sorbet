#include "core/Files.h"
#include "core/Context.h"
#include "core/GlobalState.h"
#include "core/NameHash.h"
#include <vector>

#include "absl/strings/match.h"

template class std::vector<std::shared_ptr<sorbet::core::File>>;
template class std::shared_ptr<sorbet::core::File>;
using namespace std;

namespace sorbet::core {

namespace {

constexpr auto EXTERNAL_PREFIX = "external/com_stripe_ruby_typer/"sv;

} // namespace

vector<int> findLineBreaks(string_view s) {
    vector<int> res;
    int i = -1;
    res.emplace_back(-1);
    for (auto c : s) {
        i++;
        if (c == '\n') {
            res.emplace_back(i);
        }
    }
    // We start at -1 so the last character of the file is actually i+1
    res.emplace_back(i + 1);
    ENFORCE(i + 1 == s.size());
    return res;
}

StrictLevel File::fileSigil(string_view source) {
    /*
     * StrictLevel::None: <none>
     * StrictLevel::False: # typed: false
     * StrictLevel::True: # typed: true
     * StrictLevel::Strict: # typed: strict
     * StrictLevel::String: # typed: strong
     * StrictLevel::Autogenerated: # typed: autogenerated
     */
    size_t start = 0;
    while (true) {
        start = source.find("typed:", start);
        if (start == string_view::npos) {
            return StrictLevel::None;
        }

        auto comment_start = start;
        while (comment_start > 0) {
            --comment_start;
            auto c = source[comment_start];
            if (c == ' ') {
                continue;
            } else {
                break;
            }
        }
        if (source[comment_start] != '#') {
            ++start;
            continue;
        }

        start += 6;
        while (start < source.size() && source[start] == ' ') {
            ++start;
        }

        if (start >= source.size()) {
            return StrictLevel::None;
        }
        auto end = start + 1;
        while (end < source.size() && source[end] != ' ' && source[end] != '\n') {
            ++end;
        }

        string_view suffix = source.substr(start, end - start);
        if (suffix == "ignore") {
            return StrictLevel::Ignore;
        } else if (suffix == "false") {
            return StrictLevel::False;
        } else if (suffix == "true") {
            return StrictLevel::True;
        } else if (suffix == "strict") {
            return StrictLevel::Strict;
        } else if (suffix == "strong") {
            return StrictLevel::Strong;
        } else if (suffix == "autogenerated") {
            return StrictLevel::Autogenerated;
        } else if (suffix == "__STDLIB_INTERNAL") {
            return StrictLevel::Stdlib;
        } else {
            // TODO(nelhage): We should report an error here to help catch
            // typos. This would require refactoring so this function has
            // access to GlobalState or can return errors to someone who
            // does.
        }

        start = end;
    }
}

File::File(string &&path_, string &&source_, Type sourceType, u4 epoch)
    : epoch(epoch), sourceType(sourceType), path_(move(path_)), source_(move(source_)),
      originalSigil(fileSigil(this->source_)), strictLevel(originalSigil) {}

unique_ptr<File> File::deepCopy(GlobalState &gs) const {
    string sourceCopy = source_;
    string pathCopy = path_;
    auto ret = make_unique<File>(move(pathCopy), move(sourceCopy), sourceType, epoch);
    ret->lineBreaks_ = lineBreaks_;
    ret->minErrorLevel_ = minErrorLevel_;
    ret->strictLevel = strictLevel;
    return ret;
}

void File::setFileHash(unique_ptr<const FileHash> hash) {
    // If hash_ != nullptr, then the contents of hash_ and hash should be identical.
    // Avoid needlessly invalidating references to *hash_.
    if (hash_ == nullptr) {
        cached = false;
        hash_ = move(hash);
    }
}

const shared_ptr<const FileHash> &File::getFileHash() const {
    return hash_;
}

FileRef::FileRef(unsigned int id) : _id(id) {}

const File &FileRef::data(const GlobalState &gs) const {
    ENFORCE(gs.files[_id]);
    ENFORCE(gs.files[_id]->sourceType != File::Type::TombStone);
    ENFORCE(gs.files[_id]->sourceType != File::Type::NotYetRead);
    return dataAllowingUnsafe(gs);
}

File &FileRef::data(GlobalState &gs) const {
    ENFORCE(gs.files[_id]);
    ENFORCE(gs.files[_id]->sourceType != File::Type::TombStone);
    ENFORCE(gs.files[_id]->sourceType != File::Type::NotYetRead);
    return dataAllowingUnsafe(gs);
}

const File &FileRef::dataAllowingUnsafe(const GlobalState &gs) const {
    ENFORCE(_id < gs.filesUsed());
    return *(gs.files[_id]);
}

File &FileRef::dataAllowingUnsafe(GlobalState &gs) const {
    ENFORCE(_id < gs.filesUsed());
    return *(gs.files[_id]);
}

string_view File::path() const {
    return this->path_;
}

string_view File::source() const {
    ENFORCE(this->sourceType != File::Type::TombStone);
    ENFORCE(this->sourceType != File::Type::NotYetRead);
    return this->source_;
}

StrictLevel File::minErrorLevel() const {
    return minErrorLevel_;
}

bool File::isPayload() const {
    return sourceType == File::Type::PayloadGeneration || sourceType == File::Type::Payload;
}

bool File::isRBI() const {
    return absl::EndsWith(path(), ".rbi");
}

bool File::isStdlib() const {
    return fileSigil(source()) == StrictLevel::Stdlib;
}

bool File::isPackage() const {
    return sourceType == File::Type::Package;
}

vector<int> &File::lineBreaks() const {
    ENFORCE(this->sourceType != File::Type::TombStone);
    ENFORCE(this->sourceType != File::Type::NotYetRead);
    auto ptr = atomic_load(&lineBreaks_);
    if (ptr != nullptr) {
        return *ptr;
    } else {
        auto my = make_shared<vector<int>>(findLineBreaks(this->source_));
        atomic_compare_exchange_weak(&lineBreaks_, &ptr, my);
        return lineBreaks();
    }
}

int File::lineCount() const {
    return lineBreaks().size() - 1;
}

string_view File::getLine(int i) {
    auto &lineBreaks = this->lineBreaks();
    ENFORCE(i < lineBreaks.size());
    ENFORCE(i > 0);
    auto start = lineBreaks[i - 1] + 1;
    auto end = lineBreaks[i];
    return source().substr(start, end - start);
}

string File::censorFilePathForSnapshotTests(string_view orig) {
    string_view result = orig;
    if (absl::StartsWith(result, EXTERNAL_PREFIX)) {
        // When running tests from outside of the sorbet repo, the files have a different path in the sandbox.
        result.remove_prefix(EXTERNAL_PREFIX.size());
    }

    if (absl::StartsWith(result, URL_PREFIX)) {
        // This is so that changing RBIs doesn't mean invalidating every symbol-table exp test.
        result.remove_prefix(URL_PREFIX.size());
        if (absl::StartsWith(result, EXTERNAL_PREFIX)) {
            result.remove_prefix(EXTERNAL_PREFIX.size());
        }
    }

    if (absl::StartsWith(orig, URL_PREFIX)) {
        return fmt::format("{}{}", URL_PREFIX, result);
    } else {
        return string(result);
    }
}

} // namespace sorbet::core
