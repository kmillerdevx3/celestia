// parser.cpp
//
// Copyright (C) 2001 Chris Laurel <claurel@shatters.net>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "parser.h"


/****** Value method implementations *******/

Value::Value(double d)
{
    type = NumberType;
    data.d = d;
}

Value::Value(string s)
{
    type = StringType;
    data.s = new string(s);
}

Value::Value(Array* a)
{
    type = ArrayType;
    data.a = a;
}

Value::Value(Hash* h)
{
    type = HashType;
    data.h = h;
}

Value::Value(bool b)
{
    type = BooleanType;
    data.d = b ? 1.0 : 0.0;
}

Value::~Value()
{
    if (type == StringType)
    {
        delete data.s;
    }
    else if (type == ArrayType)
    {
        if (data.a != NULL)
        {
            for (int i = 0; i < data.a->size(); i++)
                delete (*data.a)[i];
            delete data.a;
        }
    }
    else if (type == HashType)
    {
        if (data.h != NULL)
        {
#if 0
            Hash::iterator iter = data.h->begin();
            while (iter != data.h->end())
            {
                delete iter->second;
                iter++;
            }
#endif
            delete data.h;
        }
    }
}

Value::ValueType Value::getType() const
{
    return type;
}

double Value::getNumber() const
{
    // ASSERT(type == NumberType);
    return data.d;
}

string Value::getString() const
{
    // ASSERT(type == StringType);
    return *data.s;
}

Array* Value::getArray() const
{
    // ASSERT(type == ArrayType);
    return data.a;
}

Hash* Value::getHash() const
{
    // ASSERT(type == HashType);
    return data.h;
}

bool Value::getBoolean() const
{
    // ASSERT(type == BooleanType);
    return (data.d != 0.0);
}


/****** Parser method implementation ******/

Parser::Parser(Tokenizer* _tokenizer) :
    tokenizer(_tokenizer)
{
}


Array* Parser::readArray()
{
    Tokenizer::TokenType tok = tokenizer->nextToken();
    if (tok != Tokenizer::TokenBeginArray)
    {
        tokenizer->pushBack();
        return NULL;
    }

    Array* array = new Array();

    Value* v = readValue();
    while (v != NULL)
    {
        array->insert(array->end(), v);
        v = readValue();
    }
    
    tok = tokenizer->nextToken();
    if (tok != Tokenizer::TokenEndArray)
    {
        tokenizer->pushBack();
        delete array;
        return NULL;
    }

    return array;
}


Hash* Parser::readHash()
{
    Tokenizer::TokenType tok = tokenizer->nextToken();
    if (tok != Tokenizer::TokenBeginGroup)
    {
        tokenizer->pushBack();
        return NULL;
    }

    Hash* hash = new Hash();

    tok = tokenizer->nextToken();
    while (tok != Tokenizer::TokenEndGroup)
    {
        if (tok != Tokenizer::TokenName)
        {
            tokenizer->pushBack();
            delete hash;
            return NULL;
        }
        string name = tokenizer->getNameValue();

        Value* value = readValue();
        if (value == NULL)
        {
            delete hash;
            return NULL;
        }
        
        hash->addValue(name, *value);

        tok = tokenizer->nextToken();
    }

    return hash;
}


Value* Parser::readValue()
{
    Tokenizer::TokenType tok = tokenizer->nextToken();
    switch (tok)
    {
    case Tokenizer::TokenNumber:
        return new Value(tokenizer->getNumberValue());

    case Tokenizer::TokenString:
        return new Value(tokenizer->getStringValue());

    case Tokenizer::TokenName:
        if (tokenizer->getNameValue() == "false")
            return new Value(false);
        else if (tokenizer->getNameValue() == "true")
            return new Value(true);
        else
        {
            tokenizer->pushBack();
            return NULL;
        }

    case Tokenizer::TokenBeginArray:
        tokenizer->pushBack();
        {
            Array* array = readArray();
            if (array == NULL)
                return NULL;
            else
                return new Value(array);
        }

    case Tokenizer::TokenBeginGroup:
        tokenizer->pushBack();
        {
            Hash* hash = readHash();
            if (hash == NULL)
                return NULL;
            else
                return new Value(hash);
        }

    default:
        tokenizer->pushBack();
        return NULL;
    }
}


#if 0
// TODO: Move all these get* functions into a Hash class.

bool getNumber(Hash* h, string name, double& val)
{
    Hash::iterator iter = h->find(name);
    if (iter == h->end())
        return false;

    Value* v = iter->second;
    if (v->getType() != Value::NumberType)
        return false;

    val = v->getNumber();

    return true;
}

bool getNumber(Hash* h, string name, float& val)
{
    double dval;

    if (!getNumber(h, name, dval))
    {
        return false;
    }
    else
    {
        val = (float) dval;
        return true;
    }
}

bool getString(Hash* h, string name, string& val)
{
    Hash::iterator iter = h->find(name);
    if (iter == h->end())
        return false;

    Value* v = iter->second;
    if (v->getType() != Value::StringType)
        return false;

    val = v->getString();

    return true;
}

bool getBoolean(Hash* h, string name, bool& val)
{
    Hash::iterator iter = h->find(name);
    if (iter == h->end())
        return false;

    Value* v = iter->second;
    if (v->getType() != Value::BooleanType)
        return false;

    val = v->getBoolean();

    return true;
}

bool getVector(Hash* h, string name, Vec3d& val)
{
    Hash::iterator iter = h->find(name);
    if (iter == h->end())
        return false;

    Value* v = iter->second;
    if (v->getType() != Value::ArrayType)
        return false;

    Array* arr = v->getArray();
    if (arr->size() != 3)
        return false;

    Value* x = (*arr)[0];
    Value* y = (*arr)[1];
    Value* z = (*arr)[2];

    if (x->getType() != Value::NumberType ||
        y->getType() != Value::NumberType ||
        z->getType() != Value::NumberType)
        return false;

    val = Vec3d((float) x->getNumber(),
                (float) y->getNumber(),
                (float) z->getNumber());

    return true;
}

bool getVector(Hash* h, string name, Vec3f& val)
{
    Vec3d vecVal;

    if (!getVector(h, name, vecVal))
        return false;

    val = Vec3f((float) vecVal.x, (float) vecVal.y, (float) vecVal.z);

    return true;
}

bool getColor(Hash* h, string name, Color& val)
{
    Vec3d vecVal;

    if (!getVector(h, name, vecVal))
        return false;

    val = Color((float) vecVal.x, (float) vecVal.y, (float) vecVal.z);

    return true;
}
#endif


AssociativeArray::AssociativeArray()
{
}

AssociativeArray::~AssociativeArray()
{
#if 0
    Hash::iterator iter = data.h->begin();
    while (iter != data.h->end())
    {
        delete iter->second;
        iter++;
    }
#endif
    for (map<string, Value*>::iterator iter = assoc.begin(); iter != assoc.end(); iter++)
        delete iter->second;
}

Value* AssociativeArray::getValue(string key) const
{
    map<string, Value*>::const_iterator iter = assoc.find(key);
    if (iter == assoc.end())
        return NULL;
    else
        return iter->second;
}

void AssociativeArray::addValue(string key, Value& val)
{
    assoc.insert(map<string, Value*>::value_type(key, &val));
}

bool AssociativeArray::getNumber(string key, double& val) const
{
    Value* v = getValue(key);
    if (v == NULL || v->getType() != Value::NumberType)
        return false;

    val = v->getNumber();

    return true;
}

bool AssociativeArray::getNumber(string key, float& val) const
{
    double dval;

    if (!getNumber(key, dval))
    {
        return false;
    }
    else
    {
        val = (float) dval;
        return true;
    }
}

bool AssociativeArray::getString(string key, string& val) const
{
    Value* v = getValue(key);
    if (v == NULL || v->getType() != Value::StringType)
        return false;

    val = v->getString();

    return true;
}

bool AssociativeArray::getBoolean(string key, bool& val) const
{
    Value* v = getValue(key);
    if (v == NULL || v->getType() != Value::BooleanType)
        return false;

    val = v->getBoolean();

    return true;
}

bool AssociativeArray::getVector(string key, Vec3d& val) const
{
    Value* v = getValue(key);
    if (v == NULL || v->getType() != Value::ArrayType)
        return false;

    Array* arr = v->getArray();
    if (arr->size() != 3)
        return false;

    Value* x = (*arr)[0];
    Value* y = (*arr)[1];
    Value* z = (*arr)[2];

    if (x->getType() != Value::NumberType ||
        y->getType() != Value::NumberType ||
        z->getType() != Value::NumberType)
        return false;

    val = Vec3d((float) x->getNumber(),
                (float) y->getNumber(),
                (float) z->getNumber());

    return true;
}

bool AssociativeArray::getVector(string key, Vec3f& val) const
{
    Vec3d vecVal;

    if (!getVector(key, vecVal))
        return false;

    val = Vec3f((float) vecVal.x, (float) vecVal.y, (float) vecVal.z);

    return true;
}

bool AssociativeArray::getColor(string key, Color& val) const
{
    Vec3d vecVal;

    if (!getVector(key, vecVal))
        return false;

    val = Color((float) vecVal.x, (float) vecVal.y, (float) vecVal.z);

    return true;
}
