// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "formattexteditor.h"

#include "textdocument.h"
#include "textdocumentlayout.h"
#include "texteditor.h"
#include "texteditortr.h"

#include <coreplugin/messagemanager.h>

#include <utils/async.h>
#include <utils/differ.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>
#include <utils/temporarydirectory.h>
#include <utils/textutils.h>

#include <QFutureWatcher>
#include <QScrollBar>
#include <QTextBlock>

using namespace Utils;

using namespace std::chrono_literals;

namespace TextEditor {

struct FormatInput
{
    Utils::FilePath filePath;
    QString sourceData;
    TextEditor::Command command;
    int startPos = -1;
    int endPos = 0;
};

using FormatOutput = Result<QString>;

void formatCurrentFile(const Command &command, int startPos, int endPos)
{
    if (TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget())
        formatEditorAsync(editor, command, startPos, endPos);
}

static QString sourceData(TextEditorWidget *editor, int startPos, int endPos)
{
    return (startPos < 0)
            ? editor->toPlainText()
            : Utils::Text::textAt(editor->document(), startPos, (endPos - startPos));
}

static FormatOutput format(const FormatInput &input)
{
    const FilePath executable = input.command.executable();
    if (executable.isEmpty())
        return {};

    switch (input.command.processing()) {
    case Command::FileProcessing: {
        // Save text to temporary file
        Utils::TempFileSaver sourceFile(
            input.filePath.parentDir()
            / (input.filePath.fileName() + "_format_XXXXXXXX." + input.filePath.suffix()));
        sourceFile.setAutoRemove(true);
        sourceFile.write(input.sourceData.toUtf8());
        if (const Result<> res = sourceFile.finalize(); !res) {
            return Utils::make_unexpected(Tr::tr("Cannot create temporary file \"%1\": %2.")
                         .arg(sourceFile.filePath().toUserOutput(), res.error()));
        }

        // Format temporary file
        QStringList options = input.command.options();
        options.replaceInStrings(QLatin1String("%file"), sourceFile.filePath().toUrlishString());
        Process process;
        process.setCommand({executable, options});
        process.setUtf8StdOutCodec();
        process.runBlocking(5s);
        if (process.result() != ProcessResult::FinishedWithSuccess) {
            return Utils::make_unexpected(Tr::tr("Failed to format: %1.")
                                             .arg(process.exitMessage()));
        }
        const QString output = process.cleanedStdErr();
        if (!output.isEmpty())
            return Utils::make_unexpected(executable.toUserOutput() + ": " + output);

        // Read text back
        const Result<QByteArray> contents = sourceFile.filePath().fileContents();
        if (!contents) {
            return Utils::make_unexpected(Tr::tr("Cannot read file \"%1\": %2.")
                         .arg(sourceFile.filePath().toUserOutput(), contents.error()));
        }
        return QString::fromUtf8(*contents);
    }

    case Command::PipeProcessing: {
        Process process;
        QStringList options = input.command.options();
        options.replaceInStrings("%filename", input.filePath.fileName());
        options.replaceInStrings("%file", input.filePath.toUrlishString());
        process.setCommand({executable, options});
        process.setWriteData(input.sourceData.toUtf8());
        process.setUtf8StdOutCodec();
        process.start();
        if (!process.waitForFinished(5s)) {
            return Utils::make_unexpected(Tr::tr("Cannot call %1 or some other error occurred. "
                                                 "Timeout reached while formatting file %2.")
                    .arg(executable.toUserOutput(), input.filePath.displayName()));
        }
        const QString errorText = process.readAllStandardError();
        if (!errorText.isEmpty()) {
            return Utils::make_unexpected(QString("%1: %2").arg(executable.toUserOutput(),
                                                                errorText));
        }

        QString formattedData = process.readAllStandardOutput();

        if (input.command.pipeAddsNewline() && formattedData.endsWith('\n')) {
            formattedData.chop(1);
            if (formattedData.endsWith('\r'))
                formattedData.chop(1);
        }
        if (input.command.returnsCRLF())
            formattedData.replace("\r\n", "\n");

        return formattedData;
    }
    }
    return {};
}

/**
 * Sets the text of @a editor to @a text. Instead of replacing the entire text, however, only the
 * actually changed parts are updated while preserving the cursor position, the folded
 * blocks, and the scroll bar position.
 */
void updateEditorText(PlainTextEdit *editor, const QString &text)
{
    const QString editorText = editor->toPlainText();
    if (editorText == text)
        return;

    // Calculate diff
    Differ differ;
    const QList<Diff> diff = differ.diff(editorText, text);

    // Since QTextCursor does not work properly with folded blocks, all blocks must be unfolded.
    // To restore the current state at the end, keep track of which block is folded.
    QList<int> foldedBlocks;
    QTextBlock block = editor->document()->firstBlock();
    while (block.isValid()) {
        if (TextBlockUserData::isFolded(block)) {
            foldedBlocks << block.blockNumber();
            TextBlockUserData::doFoldOrUnfold(block, true);
        }
        block = block.next();
    }
    editor->update();

    // Save the current viewport position of the cursor to ensure the same vertical position after
    // the formatted text has set to the editor.
    int absoluteVerticalCursorOffset = editor->cursorRect().y();

    // Update changed lines and keep track of the cursor position
    QTextCursor cursor = editor->textCursor();
    int charactersInfrontOfCursor = cursor.position();
    int newCursorPos = charactersInfrontOfCursor;
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
    for (const Diff &d : diff) {
        switch (d.command) {
        case Diff::Insert:
        {
            // Adjust cursor position if we do work in front of the cursor.
            if (charactersInfrontOfCursor > 0) {
                const int size = d.text.size();
                charactersInfrontOfCursor += size;
                newCursorPos += size;
            }
            // Adjust folded blocks, if a new block is added.
            if (d.text.contains('\n')) {
                const int newLineCount = d.text.count('\n');
                const int number = cursor.blockNumber();
                const int total = foldedBlocks.size();
                for (int i = 0; i < total; ++i) {
                    if (foldedBlocks.at(i) > number)
                        foldedBlocks[i] += newLineCount;
                }
            }
            cursor.insertText(d.text);
            break;
        }

        case Diff::Delete:
        {
            // Adjust cursor position if we do work in front of the cursor.
            if (charactersInfrontOfCursor > 0) {
                const int size = d.text.size();
                charactersInfrontOfCursor -= size;
                newCursorPos -= size;
                // Cursor was inside the deleted text, so adjust the new cursor position
                if (charactersInfrontOfCursor < 0)
                    newCursorPos -= charactersInfrontOfCursor;
            }
            // Adjust folded blocks, if at least one block is being deleted.
            if (d.text.contains('\n')) {
                const int newLineCount = d.text.count('\n');
                const int number = cursor.blockNumber();
                for (int i = 0, total = foldedBlocks.size(); i < total; ++i) {
                    if (foldedBlocks.at(i) > number) {
                        foldedBlocks[i] -= newLineCount;
                        if (foldedBlocks[i] < number) {
                            foldedBlocks.removeAt(i);
                            --i;
                            --total;
                        }
                    }
                }
            }
            cursor.setPosition(cursor.position() + d.text.size(), QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            break;
        }

        case Diff::Equal:
            // Adjust cursor position
            charactersInfrontOfCursor -= d.text.size();
            cursor.setPosition(cursor.position() + d.text.size(), QTextCursor::MoveAnchor);
            break;
        }
    }
    cursor.endEditBlock();
    cursor.setPosition(newCursorPos);
    editor->setTextCursor(cursor);

    // Adjust vertical scrollbar
    absoluteVerticalCursorOffset = editor->cursorRect().y() - absoluteVerticalCursorOffset;
    const double fontHeight = QFontMetrics(editor->document()->defaultFont()).height();
    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->value()
                                              + absoluteVerticalCursorOffset / fontHeight);
    // Restore folded blocks
    const QTextDocument *doc = editor->document();
    for (int blockId : std::as_const(foldedBlocks)) {
        const QTextBlock block = doc->findBlockByNumber(qMax(0, blockId));
        if (block.isValid())
            TextBlockUserData::doFoldOrUnfold(block, false);
    }

    editor->document()->setModified(true);
}

static void showError(const QString &error)
{
    Core::MessageManager::writeFlashing(Tr::tr("Error in text formatting: %1").arg(error.trimmed()));
}

/**
 * Checks the state of @a task and if the formatting was successful calls updateEditorText() with
 * the respective members of @a task.
 */
static void checkAndApplyTask(const QPointer<PlainTextEdit> &textEditor, const FormatInput &input,
                              const FormatOutput &output)
{
    if (!output.has_value()) {
        showError(output.error());
        return;
    }

    if (output->isEmpty()) {
        showError(Tr::tr("Could not format file %1.").arg(input.filePath.displayName()));
        return;
    }

    if (!textEditor) {
        showError(Tr::tr("File %1 was closed.").arg(input.filePath.displayName()));
        return;
    }

    const QString formattedData = (input.startPos < 0) ? *output
        : textEditor->toPlainText().replace(input.startPos, (input.endPos - input.startPos), *output);
    updateEditorText(textEditor, formattedData);
}

/**
 * Formats the text of @a editor using @a command. @a startPos and @a endPos specifies the range of
 * the editor's text that will be formatted. If @a startPos is negative the editor's entire text is
 * formatted.
 *
 * @pre @a endPos must be greater than or equal to @a startPos
 */
void formatEditor(TextEditorWidget *editor, const Command &command, int startPos, int endPos)
{
    QTC_ASSERT(startPos <= endPos, return);

    const QString sd = sourceData(editor, startPos, endPos);
    if (sd.isEmpty())
        return;
    const FormatInput input{editor->textDocument()->filePath(), sd, command, startPos, endPos};
    checkAndApplyTask(editor, input, format(input));
}

/**
 * Behaves like formatEditor except that the formatting is done asynchronously.
 */
void formatEditorAsync(TextEditorWidget *editor, const Command &command, int startPos, int endPos)
{
    QTC_ASSERT(startPos <= endPos, return);

    const QString sd = sourceData(editor, startPos, endPos);
    if (sd.isEmpty())
        return;

    auto watcher = new QFutureWatcher<FormatOutput>;
    const TextDocument *doc = editor->textDocument();
    const FormatInput input{doc->filePath(), sd, command, startPos, endPos};
    QObject::connect(doc, &TextDocument::contentsChanged, watcher,
                     &QFutureWatcher<FormatOutput>::cancel);
    QObject::connect(watcher, &QFutureWatcherBase::finished, watcher,
                     [watcher, editor = QPointer<PlainTextEdit>(editor), input] {
        if (watcher->isCanceled())
            showError(Tr::tr("File was modified."));
        else
            checkAndApplyTask(editor, input, watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(Utils::asyncRun(&format, input));
}

} // namespace TextEditor

#ifdef WITH_TESTS

#include <QTest>

namespace TextEditor::Internal {

class FormatTextTest final : public QObject
{
    Q_OBJECT

private slots:
    void testFormatting_data();
    void testFormatting();
};

void FormatTextTest::testFormatting_data()
{
    QTest::addColumn<QString>("code");
    QTest::addColumn<QString>("result");

    {
        QString code {
            "import QtQuick\n\n"
            "  Item {\n"
            "     property string cat: [\"👩🏽‍🚒d👩🏽‍🚒d👩🏽‍🚒\"]\n"
            "        property string dog: cat\n"
            "}\n"
        };

        QString result {
            "import QtQuick\n\n"
            "Item {\n"
            "    property string cat: [\"👩🏽‍🚒\"]\n"
            "    property string dog: cat\n"
            "}\n"
        };

        QTest::newRow("unicodeCharacterInFormattedCode") << code << result;
    }
}

void FormatTextTest::testFormatting()
{
    QFETCH(QString, code);
    QFETCH(QString, result);

    QScopedPointer<TextEditorWidget> editor(new TextEditorWidget);
    QVERIFY(editor.get());

    QSharedPointer<TextDocument> doc(new TextDocument);
    doc->setPlainText(code);
    editor->setTextDocument(doc);

    TextEditor::updateEditorText(editor.get(), result);

    QCOMPARE(editor->toPlainText(), result);
}

QObject *createFormatTextTest()
{
    return new FormatTextTest;
}

} // namespace TextEditor::Internal

#include "formattexteditor.moc"

#endif // WITH_TESTS
