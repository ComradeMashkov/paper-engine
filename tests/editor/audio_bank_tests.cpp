#include "../../editor/audio_bank_editor.hpp"
#include <QApplication>
#include <QFile>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <iostream>
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) {
            ++failures;
            std::cerr << message << '\n';
        }
    };
    auto rejects = [](auto action) {
        try {
            action();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    QTemporaryDir directory;
    const auto root = std::filesystem::path(directory.path().toStdString());
    const auto file = root / "audio.pabank";
    const QByteArray original = "format='paper.audio'\nversion=1\n# authored "
                                "comment\n[bank]\nsounds=[]\nsources=[]\nzones=[]\n";
    auto write = [&](const QByteArray& text) {
        QFile out(QString::fromStdString(file.string()));
        out.open(QIODevice::WriteOnly);
        out.write(text);
    };
    auto read = [&] {
        QFile in(QString::fromStdString(file.string()));
        in.open(QIODevice::ReadOnly);
        return in.readAll();
    };
    write(original);
    paper::editor::AudioBankEditor editor(file, root);
    auto* source = editor.findChild<QPlainTextEdit*>("audioBankSource");
    check(source, "source control exists");
    editor.validate();
    editor.save();
    check(read() == original, "no-op bank Save preserves original bytes/comments");
    source->appendPlainText("# edited");
    editor.save();
    check(read().contains("# authored comment") && read().contains("# edited"),
          "bank source Save preserves user content");
    const auto saved = read();
    source->setPlainText("invalid");
    check(rejects([&] { editor.save(); }) && read() == saved,
          "invalid bank cannot replace the saved document");
    source->setPlainText(QString::fromUtf8(saved));
    write(original);
    check(rejects([&] { editor.save(); }) && read() == original,
          "external changes cannot be overwritten");
    std::cout << "audio editor failures=" << failures << '\n';
    return failures ? 1 : 0;
}
