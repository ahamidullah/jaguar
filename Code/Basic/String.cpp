#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"

char &String::operator[](size_t i)
{
	Assert(i < data.count);
	return data[i];
}

char &String::operator[](size_t i) const
{
	Assert(i < data.count);
	return data[i];
}

bool operator==(const String &a, const String &b)
{
	return a.data == b.data;
}

bool operator!=(const String &a, const String &b)
{
	return a.data != b.data;
}

bool CStringsEqual(const char *a, const char *b)
{
	while (*a && *b)
	{
		if (*a != *b)
		{
			return false;
		}
		a++;
		b++;
	}
	if (*a || *b)
	{
		return false;
	}
	return true;
}

String CreateString(size_t length, size_t capacity)
{
	String result = {
		.data = CreateArray<char>(length + 1, capacity + 1),
	};
	result.data[length] = '\0';
	return result;
}

String CreateString(size_t length)
{
	String result = {
		.data = CreateArray<char>(length + 1),
	};
	result.data[length] = '\0';
	return result;
}

String CreateString(const String &copy)
{
	auto length = Length(copy);
	String result = {
		.data = CreateArray<char>(length + 1),
	};
	CopyMemory(&copy[0], &result[0], length);
	result[length] = '\0';
	return result;
}

String CreateString(const String &copy, size_t startIndex, size_t endIndex)
{
	Assert(endIndex >= startIndex);
	auto length = endIndex - startIndex + 1;
	auto result = CreateString(length);
	CopyMemory(&copy[startIndex], &result[0], length);
	return result;
}

void Resize(String *string, size_t newSize)
{
	Resize(&string->data, newSize + 1);
	string->data[newSize] = '\0';
}

void Append(String *Destination, const String &Source)
{
	size_t WriteIndex = Length(Destination);
	Resize(&Destination->data, Length(Destination) + Length(Source) + 1);
	CopyMemory(&Source.data[0], &Destination->data[WriteIndex], Length(Source) + 1);
}

size_t Length(const char *string)
{
	size_t length = 0;
	while (string[length])
	{
		length++;
	}
	return length;
}

void Append(String *Destination, const char *Source)
{
	size_t SourceLength = Length(Source);
	size_t WriteIndex = Length(Destination);
	Resize(&Destination->data, Length(Destination) + SourceLength + 1);
	CopyMemory(Source, &Destination->data[WriteIndex], SourceLength + 1);
}

void Append(String *Destination, String Source, u32 RangeStartIndex, u32 RangeLength)
{
	Assert(Source.data.count > RangeStartIndex);
	Assert(Source.data.count >= RangeStartIndex + RangeLength);
	size_t WriteIndex = Length(Destination);
	Resize(&Destination->data, Length(Destination) + RangeLength + 1);
	CopyMemory(&Source.data[RangeStartIndex], &Destination->data[WriteIndex], RangeLength + 1);

	///SetSize(&Destination->data, Length(Destination->data) - 1);
	///Append(&Destination->data, &Source.data[RangeStartIndex], RangeLength);
	///Append(&Destinaton->data, '\0');
}

void Append(String *s, char c)
{
	Resize(&s->data, Length(s) + 2);
	s->data[Length(s->data) - 2] = c;
	s->data[Length(s->data) - 1] = '\0';
}

size_t Length(const String *S)
{
	if (Length(S->data) == 0)
	{
		return 0;
	}
	return Length(S->data) - 1;
}

size_t Length(const String &S)
{
	if (Length(S.data) == 0)
	{
		return 0;
	}
	return Length(S.data) - 1; // To account for the NULL terminator we insert at the end.
}

char *begin(String &S)
{
	return &S.data[0];
}

char *end(String &S)
{
	return &S.data[S.data.count];
}

char *begin(String *S)
{
	return &S->data[0];
}

char *end(String *S)
{
	return &S->data[S->data.count];
}

s32 FormatString(char *buffer, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	auto numberOfBytesWritten = stbsp_vsprintf(buffer, format, arguments);
	va_end(arguments);
	return numberOfBytesWritten;
}

s32 FormatString(char *buffer, const char *format, va_list arguments)
{
	return stbsp_vsprintf(buffer, format, arguments);
}

String _FormatString(const char *format, ...)
{
	char buffer[2056]; // @TODO
	va_list arguments;
	va_start(arguments, format);
	FormatString(buffer, format, arguments);
	va_end(arguments);
	return CreateString(buffer);
}

s64 FindFirstIndex(const String &s, char c)
{
	for (size_t i = 0; i < Length(s); i++)
	{
		if (s.data[i] == c)
		{
			return i;
		}
	}
	return -1;
}

s64 FindLastIndex(const String &s, char c)
{
	s64 occurrence = -1;
	for (size_t i = 0; i < Length(s); i++)
	{
		if (s.data[i] == c)
		{
			occurrence = i;
		}
	}
	return occurrence;
}

bool IsSpace(char c)
{
	if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
	{
		return true;
	}
	return false;
}

void Trim(String *s, size_t leftIndex, size_t rightIndex)
{
	Assert(rightIndex > leftIndex);
	auto length = rightIndex - (leftIndex + 1);
	MoveMemory(&s->data[leftIndex + 1], &s->data[0], length);
	Resize(s, length);
}

Array<String> Split(const String &s, char seperator)
{
	Array<String> result;
	auto splitStartIndex = 0;
	auto splitLength = 0;
	for (auto i = 0; i < Length(s); i++)
	{
		if (s[i] == seperator)
		{
			if (splitLength > 0)
			{
				Append(&result, CreateString(s, splitStartIndex, i - 1));
				splitLength = 0;
			}
		}
		else
		{
			if (splitLength == 0)
			{
				splitStartIndex = i;
			}
			splitLength++;
		}
	}
	if (splitLength > 0)
	{
		Append(&result, CreateString(s, splitStartIndex, Length(s) - 1));
	}
	return result;
}