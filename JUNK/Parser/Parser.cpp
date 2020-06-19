#include "Parser.h"
#include "File.h"
#include "Assert.h"

Parser CreateParser(const String &filepath, const String &delimiters, bool *error)
{
	auto fileString = ReadEntireFile(filepath, error);
	if (*error)
	{
		return Parser{};
	}
	*error = false;
	return
	{
		.string = fileString,
		.delimiters = delimiters,
	};
}

bool IsParserDelimiter(Parser *parser, char c)
{
	for (auto d : parser->delimiters)
	{
		if (c == d)
		{
			return true;
		}
	}
	return false;
}

void AdvanceParser(Parser *parser)
{
	if (parser->index >= StringLength(parser->string))
	{
		return;
	}
	if (parser->string[parser->index] == '\n')
	{
		parser->line++;
		parser->column = 0;
	}
	parser->index++;
	parser->column++;
}

String GetParserToken(Parser *parser)
{
	while (parser->index < StringLength(parser->string) && (parser->string[parser->index] == ' ' || parser->string[parser->index] == '\t'))
	{
		AdvanceParser(parser);
	}
	if (parser->index >= StringLength(parser->string))
	{
		return "";
	}
	auto tokenStartIndex = parser->index;
	if (IsParserDelimiter(parser, parser->string[parser->index]))
	{
		AdvanceParser(parser);
	}
	else
	{
		while (parser->index < StringLength(parser->string) && !IsParserDelimiter(parser, parser->string[parser->index]))
		{
			AdvanceParser(parser);
		}
	}
	Assert(parser->index > tokenStartIndex);
	return CreateString(parser->string, tokenStartIndex, parser->index - 1);
}

String GetParserLine(Parser *parser)
{
	if (parser->index >= StringLength(parser->string))
	{
		return "";
	}
	auto lineStartIndex = parser->index;
	while (parser->index < StringLength(parser->string) && parser->string[parser->index] != '\n')
	{
		AdvanceParser(parser);
	}
	if (parser->string[parser->index] == '\n')
	{
		AdvanceParser(parser);
	}
	Assert(parser->index > lineStartIndex);
	return CreateString(parser->string, lineStartIndex, parser->index - 1);
}

/*
String ParserGetUntilChar(Parser *parser, char c)
{
	auto startIndex = parser->index;
	while (parser->string[parser->index] && parser->string[parser->index] != c)
	{
		AdvanceParser(parser);
	}
	if (!parser->string[parser->index] || startIndex == parser->index)
	{
		return "";
	}
	return CreateString(parser->string, startIndex, parser->index - 1);
}

bool GetIfParserToken(Parser *parser, const String &expected)
{
	auto initialParser = *parser;
	auto token = GetParserToken(parser);
	if (token == expected)
	{
		return true;
	}
	*parser = initialParser;
	return false;
}

String GetParserLine(Parser *parser)
{
	auto lineStartIndex = parser->index
	while (parser->string[parser->index] && parser->string[parser->index] != '\n')
	{
		AdvanceParser(parser);
	}
	if (parser->string[parser->index] != '\n')
	{
		AdvanceParser(parser);
	}
	return CreateString(parser->string, lineStartIndex, parser->index - 1);
}

String ParserGetUntilCharOrEnd(Parser *parser, char c)
{
	auto startIndex = parser->index;
	while (parser->string[parser->index] && parser->string[parser->index] != c)
	{
		AdvanceParser(parser);
	}
	return CreateString(parser->string, startIndex, parser->index - 1);
}

bool GetUntilEndOfLine(Parser *parser, String *line)
{
	ResizeString(line, 0);
	if (!parser->string[parser->index])
	{
		return false;
	}
	while (parser->string[parser->index] && parser->string[parser->index] != '\n')
	{
		StringAppend(line, parser->string[parser->index]);
		AdvanceParser(parser);
	}
	if (parser->string[parser->index] == '\n')
	{
		StringAppend(line, parser->string[parser->index]);
		AdvanceParser(parser);
	}
	return true;
}
*/