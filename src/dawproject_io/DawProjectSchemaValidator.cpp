#include "DawProjectSchemaValidator.h"

#include <StudioDuoDawProjectSchemaData.h>

#include <juce_cryptography/juce_cryptography.h>

#include <charconv>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace studio
{
namespace
{
constexpr auto xsdPrefix = "xs:";

struct SimpleRule
{
    juce::String baseType { "xs:string" };
    bool list = false;
    juce::String listItemType;
    juce::StringArray enumeration;
};

struct ComplexRule;

struct ElementRule
{
    juce::String name;
    juce::String typeName;
    std::shared_ptr<ComplexRule> inlineComplex;
    std::shared_ptr<SimpleRule> inlineSimple;
};

struct Particle
{
    enum class Kind
    {
        element,
        sequence,
        choice
    };

    Kind kind = Kind::sequence;
    int minOccurs = 1;
    int maxOccurs = 1;
    ElementRule element;
    std::vector<Particle> children;
};

struct AttributeRule
{
    juce::String name;
    juce::String typeName { "xs:string" };
    bool required = false;
    std::shared_ptr<SimpleRule> inlineSimple;
};

struct ComplexRule
{
    juce::String name;
    juce::String baseType;
    bool abstract = false;
    std::optional<Particle> particle;
    std::vector<AttributeRule> attributes;
};

struct Schema
{
    std::map<juce::String, ElementRule> elements;
    std::map<juce::String, ComplexRule> complexTypes;
    std::map<juce::String, SimpleRule> simpleTypes;
};

struct SchemaState
{
    std::optional<Schema> schema;
    juce::String error;
    const char* data = nullptr;
    int dataSize = 0;
};

bool isSchemaTag(const juce::XmlElement& element, const char* localName)
{
    return element.hasTagName(juce::String(xsdPrefix) + localName);
}

std::vector<const juce::XmlElement*> elementChildren(
    const juce::XmlElement& parent)
{
    std::vector<const juce::XmlElement*> children;
    for (auto* child = parent.getFirstChildElement();
         child != nullptr;
         child = child->getNextElement())
    {
        if (!child->isTextElement())
            children.push_back(child);
    }
    return children;
}

int occurrenceValue(const juce::XmlElement& element,
                    const char* attribute,
                    int fallback)
{
    if (!element.hasAttribute(attribute))
        return fallback;
    const auto value = element.getStringAttribute(attribute);
    if (value == "unbounded")
        return -1;
    return value.getIntValue();
}

SimpleRule parseSimpleRule(const juce::XmlElement& element)
{
    SimpleRule rule;
    for (const auto* child : elementChildren(element))
    {
        if (isSchemaTag(*child, "restriction"))
        {
            rule.baseType = child->getStringAttribute("base", "xs:string");
            for (const auto* value : elementChildren(*child))
            {
                if (isSchemaTag(*value, "enumeration"))
                    rule.enumeration.add(
                        value->getStringAttribute("value"));
            }
        }
        else if (isSchemaTag(*child, "list"))
        {
            rule.list = true;
            rule.listItemType =
                child->getStringAttribute("itemType", "xs:string");
        }
    }
    return rule;
}

std::shared_ptr<ComplexRule> parseComplexRule(
    const juce::XmlElement& element);

ElementRule parseElementRule(const juce::XmlElement& element)
{
    ElementRule rule;
    rule.name = element.getStringAttribute("name");
    if (rule.name.isEmpty())
        rule.name = element.getStringAttribute("ref");
    rule.typeName = element.getStringAttribute("type");
    for (const auto* child : elementChildren(element))
    {
        if (isSchemaTag(*child, "complexType"))
            rule.inlineComplex = parseComplexRule(*child);
        else if (isSchemaTag(*child, "simpleType"))
            rule.inlineSimple =
                std::make_shared<SimpleRule>(parseSimpleRule(*child));
    }
    return rule;
}

Particle parseParticle(const juce::XmlElement& element)
{
    Particle particle;
    particle.minOccurs = occurrenceValue(element, "minOccurs", 1);
    particle.maxOccurs = occurrenceValue(element, "maxOccurs", 1);
    if (isSchemaTag(element, "element"))
    {
        particle.kind = Particle::Kind::element;
        particle.element = parseElementRule(element);
        return particle;
    }

    particle.kind = isSchemaTag(element, "choice")
        ? Particle::Kind::choice
        : Particle::Kind::sequence;
    for (const auto* child : elementChildren(element))
    {
        if (isSchemaTag(*child, "element")
            || isSchemaTag(*child, "sequence")
            || isSchemaTag(*child, "choice"))
        {
            particle.children.push_back(parseParticle(*child));
        }
    }
    return particle;
}

AttributeRule parseAttributeRule(const juce::XmlElement& element)
{
    AttributeRule rule;
    rule.name = element.getStringAttribute("name");
    rule.typeName = element.getStringAttribute("type", "xs:string");
    rule.required = element.getStringAttribute("use") == "required";
    for (const auto* child : elementChildren(element))
    {
        if (isSchemaTag(*child, "simpleType"))
            rule.inlineSimple =
                std::make_shared<SimpleRule>(parseSimpleRule(*child));
    }
    return rule;
}

void parseComplexBody(const juce::XmlElement& element, ComplexRule& rule)
{
    for (const auto* child : elementChildren(element))
    {
        if (isSchemaTag(*child, "sequence")
            || isSchemaTag(*child, "choice"))
        {
            auto particle = parseParticle(*child);
            if (!particle.children.empty())
                rule.particle = std::move(particle);
        }
        else if (isSchemaTag(*child, "attribute"))
        {
            rule.attributes.push_back(parseAttributeRule(*child));
        }
        else if (isSchemaTag(*child, "complexContent"))
        {
            for (const auto* content : elementChildren(*child))
            {
                if (!isSchemaTag(*content, "extension"))
                    continue;
                rule.baseType = content->getStringAttribute("base");
                parseComplexBody(*content, rule);
            }
        }
    }
}

std::shared_ptr<ComplexRule> parseComplexRule(
    const juce::XmlElement& element)
{
    auto rule = std::make_shared<ComplexRule>();
    rule->name = element.getStringAttribute("name");
    rule->abstract =
        element.getStringAttribute("abstract").equalsIgnoreCase("true");
    parseComplexBody(element, *rule);
    return rule;
}

std::optional<Schema> parseSchema(const char* data,
                                  int dataSize,
                                  juce::String& error)
{
    if (data == nullptr || dataSize <= 0)
    {
        error = "The embedded schema resource is missing.";
        return std::nullopt;
    }

    juce::XmlDocument document(juce::String::fromUTF8(data, dataSize));
    auto root = document.getDocumentElement();
    if (root == nullptr || !isSchemaTag(*root, "schema"))
    {
        error = document.getLastParseError().isNotEmpty()
            ? document.getLastParseError()
            : juce::String("The embedded schema is not an XML Schema.");
        return std::nullopt;
    }

    Schema schema;
    for (const auto* child : elementChildren(*root))
    {
        if (isSchemaTag(*child, "simpleType"))
        {
            const auto name = child->getStringAttribute("name");
            if (name.isNotEmpty())
                schema.simpleTypes.emplace(name, parseSimpleRule(*child));
        }
        else if (isSchemaTag(*child, "complexType"))
        {
            auto rule = parseComplexRule(*child);
            if (rule->name.isNotEmpty())
                schema.complexTypes.emplace(rule->name, *rule);
        }
        else if (isSchemaTag(*child, "element"))
        {
            auto rule = parseElementRule(*child);
            if (rule.name.isNotEmpty())
                schema.elements.emplace(rule.name, std::move(rule));
        }
    }
    return schema;
}

std::pair<const char*, int> resourceFor(const juce::String& fileName)
{
    for (int index = 0;
         index < studio_dawproject_schema::namedResourceListSize;
         ++index)
    {
        if (juce::String(
                studio_dawproject_schema::originalFilenames[index])
                .endsWithIgnoreCase(fileName))
        {
            int size = 0;
            const auto* data =
                studio_dawproject_schema::getNamedResource(
                    studio_dawproject_schema::namedResourceList[index],
                    size);
            return { data, size };
        }
    }
    return {};
}

SchemaState loadSchema(const juce::String& fileName)
{
    SchemaState state;
    const auto [data, size] = resourceFor(fileName);
    state.data = data;
    state.dataSize = size;
    state.schema = parseSchema(data, size, state.error);
    return state;
}

const SchemaState& projectSchemaState()
{
    static const auto state = loadSchema("Project.xsd");
    return state;
}

const SchemaState& metadataSchemaState()
{
    static const auto state = loadSchema("MetaData.xsd");
    return state;
}

bool isNcName(const juce::String& value)
{
    if (value.isEmpty())
        return false;
    auto text = value.getCharPointer();
    const auto first = text.getAndAdvance();
    if (!(first == '_' || juce::CharacterFunctions::isLetter(first)))
        return false;
    while (!text.isEmpty())
    {
        const auto character = text.getAndAdvance();
        if (!(juce::CharacterFunctions::isLetterOrDigit(character)
              || character == '_'
              || character == '-'
              || character == '.'))
        {
            return false;
        }
    }
    return true;
}

bool parseInteger(const juce::String& value)
{
    const auto text = value.trim().toStdString();
    if (text.empty())
        return false;
    int parsed = 0;
    const auto conversion = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed);
    return conversion.ec == std::errc {}
        && conversion.ptr == text.data() + text.size();
}

bool parseDouble(const juce::String& value)
{
    const auto text = value.trim().toStdString();
    if (text == "INF" || text == "-INF" || text == "NaN")
        return true;
    if (text.empty())
        return false;
    char* end = nullptr;
    std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size();
}

class Validator
{
public:
    Validator(const Schema& sourceSchema,
              juce::String requiredRoot)
        : schema(sourceSchema),
          rootName(std::move(requiredRoot))
    {
    }

    DawProjectValidationResult validate(const juce::String& xml)
    {
        result.valid = false;
        if (xml.containsIgnoreCase("<!DOCTYPE")
            || xml.containsIgnoreCase("<!ENTITY"))
        {
            addIssue("/", "DTD and entity declarations are not allowed.");
            return result;
        }

        juce::XmlDocument document(xml);
        auto root = document.getDocumentElement();
        if (root == nullptr)
        {
            addIssue(
                "/",
                document.getLastParseError().isNotEmpty()
                    ? document.getLastParseError()
                    : juce::String("The document is not valid XML."));
            return result;
        }
        if (!root->hasTagName(rootName))
        {
            addIssue(
                "/",
                "Expected root element " + rootName + ", found "
                    + root->getTagName() + ".");
            return result;
        }

        const auto declaration = schema.elements.find(rootName);
        if (declaration == schema.elements.cend())
        {
            addIssue("/", "The official schema does not declare " + rootName + ".");
            return result;
        }
        validateElement(*root, declaration->second, "/" + rootName);
        for (const auto& reference : idReferences)
        {
            if (ids.find(reference.first) == ids.cend())
            {
                addIssue(
                    reference.second,
                    "IDREF '" + reference.first
                        + "' does not identify an element in the document.");
            }
        }
        result.valid = result.issues.empty();
        return result;
    }

private:
    struct TypeDefinition
    {
        const ComplexRule* complex = nullptr;
        const SimpleRule* simple = nullptr;
        juce::String builtinSimple;
    };

    const Schema& schema;
    juce::String rootName;
    DawProjectValidationResult result;
    std::map<juce::String, juce::String> ids;
    std::vector<std::pair<juce::String, juce::String>> idReferences;

    void addIssue(const juce::String& path, const juce::String& message)
    {
        result.issues.push_back({ path, message });
    }

    TypeDefinition resolveType(const juce::String& typeName) const
    {
        if (typeName.startsWith(xsdPrefix))
            return { nullptr, nullptr, typeName };
        if (const auto complex = schema.complexTypes.find(typeName);
            complex != schema.complexTypes.cend())
            return { &complex->second, nullptr, {} };
        if (const auto simple = schema.simpleTypes.find(typeName);
            simple != schema.simpleTypes.cend())
            return { nullptr, &simple->second, {} };
        return {};
    }

    bool derivesFrom(const juce::String& actualType,
                     const juce::String& requiredBase,
                     std::set<juce::String>& visiting) const
    {
        if (actualType == requiredBase)
            return true;
        if (!visiting.insert(actualType).second)
            return false;
        const auto actual = schema.complexTypes.find(actualType);
        if (actual == schema.complexTypes.cend()
            || actual->second.baseType.isEmpty())
            return false;
        return derivesFrom(
            actual->second.baseType,
            requiredBase,
            visiting);
    }

    std::vector<AttributeRule> effectiveAttributes(
        const ComplexRule& rule,
        std::set<juce::String>& visiting) const
    {
        std::vector<AttributeRule> attributes;
        if (rule.baseType.isNotEmpty()
            && !rule.baseType.startsWith(xsdPrefix)
            && visiting.insert(rule.baseType).second)
        {
            if (const auto base = schema.complexTypes.find(rule.baseType);
                base != schema.complexTypes.cend())
            {
                attributes = effectiveAttributes(base->second, visiting);
            }
        }
        for (const auto& attribute : rule.attributes)
        {
            const auto existing = std::find_if(
                attributes.begin(),
                attributes.end(),
                [&attribute](const auto& candidate)
                {
                    return candidate.name == attribute.name;
                });
            if (existing == attributes.end())
                attributes.push_back(attribute);
            else
                *existing = attribute;
        }
        return attributes;
    }

    std::optional<Particle> effectiveParticle(
        const ComplexRule& rule,
        std::set<juce::String>& visiting) const
    {
        std::optional<Particle> baseParticle;
        if (rule.baseType.isNotEmpty()
            && !rule.baseType.startsWith(xsdPrefix)
            && visiting.insert(rule.baseType).second)
        {
            if (const auto base = schema.complexTypes.find(rule.baseType);
                base != schema.complexTypes.cend())
            {
                baseParticle = effectiveParticle(base->second, visiting);
            }
        }
        if (!baseParticle.has_value())
            return rule.particle;
        if (!rule.particle.has_value())
            return baseParticle;

        Particle combined;
        combined.kind = Particle::Kind::sequence;
        combined.children.push_back(*baseParticle);
        combined.children.push_back(*rule.particle);
        return combined;
    }

    static juce::String elementName(const ElementRule& rule)
    {
        return rule.name;
    }

    juce::String childPath(
        const juce::String& parentPath,
        const std::vector<const juce::XmlElement*>& children,
        std::size_t index) const
    {
        const auto name = children[index]->getTagName();
        auto occurrence = 0;
        for (std::size_t candidate = 0; candidate <= index; ++candidate)
            if (children[candidate]->hasTagName(name))
                ++occurrence;
        return parentPath + "/" + name + "[" + juce::String(occurrence)
            + "]";
    }

    bool canStart(const Particle& particle,
                  const juce::XmlElement& child) const
    {
        if (particle.kind == Particle::Kind::element)
            return child.hasTagName(elementName(particle.element));
        if (particle.kind == Particle::Kind::choice)
        {
            return std::any_of(
                particle.children.cbegin(),
                particle.children.cend(),
                [&child, this](const auto& option)
                {
                    return canStart(option, child);
                });
        }
        for (const auto& entry : particle.children)
        {
            if (canStart(entry, child))
                return true;
            if (entry.minOccurs > 0)
                return false;
        }
        return false;
    }

    juce::String expectedDescription(const Particle& particle) const
    {
        if (particle.kind == Particle::Kind::element)
            return elementName(particle.element);
        juce::StringArray names;
        for (const auto& child : particle.children)
        {
            if (child.kind == Particle::Kind::element)
                names.add(elementName(child.element));
            else
                names.add(expectedDescription(child));
        }
        return names.joinIntoString(" or ");
    }

    int matchOccurrences(
        const Particle& particle,
        const std::vector<const juce::XmlElement*>& children,
        std::size_t& index,
        const juce::String& parentPath)
    {
        auto count = 0;
        while ((particle.maxOccurs < 0 || count < particle.maxOccurs)
               && index < children.size()
               && canStart(particle, *children[index]))
        {
            const auto before = index;
            matchOnce(particle, children, index, parentPath);
            if (index == before)
                break;
            ++count;
        }
        if (count < particle.minOccurs)
        {
            addIssue(
                parentPath,
                "Expected " + expectedDescription(particle)
                    + " in the schema sequence.");
        }
        return count;
    }

    void matchOnce(
        const Particle& particle,
        const std::vector<const juce::XmlElement*>& children,
        std::size_t& index,
        const juce::String& parentPath)
    {
        if (particle.kind == Particle::Kind::element)
        {
            if (index >= children.size()
                || !children[index]->hasTagName(
                    elementName(particle.element)))
            {
                return;
            }
            const auto path = childPath(parentPath, children, index);
            validateElement(*children[index], particle.element, path);
            ++index;
            return;
        }
        if (particle.kind == Particle::Kind::sequence)
        {
            for (const auto& entry : particle.children)
                matchOccurrences(entry, children, index, parentPath);
            return;
        }
        if (index >= children.size())
            return;
        for (const auto& option : particle.children)
        {
            if (!canStart(option, *children[index]))
                continue;
            matchOnce(option, children, index, parentPath);
            return;
        }
        addIssue(
            childPath(parentPath, children, index),
            "Expected " + expectedDescription(particle)
                + " in the schema choice.");
    }

    bool validateSimple(const juce::String& typeName,
                        const SimpleRule* inlineRule,
                        const juce::String& value,
                        const juce::String& path)
    {
        if (inlineRule != nullptr)
            return validateSimpleRule(*inlineRule, value, path);
        const auto type = resolveType(typeName);
        if (type.simple != nullptr)
            return validateSimpleRule(*type.simple, value, path);
        const auto builtin = type.builtinSimple.isNotEmpty()
            ? type.builtinSimple
            : typeName;
        if (builtin == "xs:string")
            return true;
        if (builtin == "xs:boolean")
        {
            if (value == "true"
                || value == "false"
                || value == "1"
                || value == "0")
                return true;
        }
        else if (builtin == "xs:int")
        {
            if (parseInteger(value))
                return true;
        }
        else if (builtin == "xs:double")
        {
            if (parseDouble(value))
                return true;
        }
        else if (builtin == "xs:ID" || builtin == "xs:IDREF")
        {
            if (isNcName(value))
                return true;
        }
        addIssue(path, "Value '" + value + "' is not valid for " + builtin + ".");
        return false;
    }

    bool validateSimpleRule(const SimpleRule& rule,
                            const juce::String& value,
                            const juce::String& path)
    {
        if (rule.list)
        {
            const auto values = juce::StringArray::fromTokens(
                value,
                " \t\r\n",
                {});
            for (const auto& item : values)
                if (!validateSimple(rule.listItemType, nullptr, item, path))
                    return false;
            return true;
        }
        if (!rule.enumeration.isEmpty()
            && !rule.enumeration.contains(value))
        {
            addIssue(
                path,
                "Value '" + value + "' is not one of: "
                    + rule.enumeration.joinIntoString(", ") + ".");
            return false;
        }
        return validateSimple(rule.baseType, nullptr, value, path);
    }

    void validateAttributes(const juce::XmlElement& element,
                            const ComplexRule& rule,
                            const juce::String& path)
    {
        std::set<juce::String> visiting;
        const auto attributes = effectiveAttributes(rule, visiting);
        for (const auto& attribute : attributes)
        {
            if (attribute.required
                && !element.hasAttribute(attribute.name))
            {
                addIssue(
                    path,
                    "Missing required attribute '" + attribute.name + "'.");
            }
        }

        for (auto index = 0; index < element.getNumAttributes(); ++index)
        {
            const auto name = element.getAttributeName(index);
            if (name == "xmlns"
                || name.startsWith("xmlns:")
                || name == "xsi:type"
                || name == "xsi:noNamespaceSchemaLocation")
            {
                continue;
            }
            const auto match = std::find_if(
                attributes.cbegin(),
                attributes.cend(),
                [&name](const auto& attribute)
                {
                    return attribute.name == name;
                });
            if (match == attributes.cend())
            {
                addIssue(path + "/@" + name, "Attribute is not declared by the schema.");
                continue;
            }
            const auto value = element.getAttributeValue(index);
            if (!validateSimple(
                    match->typeName,
                    match->inlineSimple.get(),
                    value,
                    path + "/@" + name))
            {
                continue;
            }
            if (match->typeName == "xs:ID")
            {
                if (const auto existing = ids.find(value);
                    existing != ids.cend())
                {
                    addIssue(
                        path + "/@" + name,
                        "Duplicate XML ID '" + value + "'; first declared at "
                            + existing->second + ".");
                }
                else
                {
                    ids.emplace(value, path + "/@" + name);
                }
            }
            else if (match->typeName == "xs:IDREF")
            {
                idReferences.emplace_back(
                    value,
                    path + "/@" + name);
            }
        }
    }

    void validateComplex(const juce::XmlElement& element,
                         const ComplexRule& rule,
                         const juce::String& path)
    {
        if (rule.abstract)
        {
            addIssue(path, "The element uses an abstract schema type.");
            return;
        }
        validateAttributes(element, rule, path);
        const auto children = elementChildren(element);
        std::set<juce::String> visiting;
        const auto particle = effectiveParticle(rule, visiting);
        if (!particle.has_value())
        {
            if (!children.empty())
                addIssue(path, "The schema type does not allow child elements.");
            return;
        }
        std::size_t index = 0;
        matchOnce(*particle, children, index, path);
        while (index < children.size())
        {
            addIssue(
                childPath(path, children, index),
                "Element is not allowed at this position by the schema.");
            ++index;
        }
    }

    void validateElement(const juce::XmlElement& element,
                         const ElementRule& declaration,
                         const juce::String& path)
    {
        if (declaration.inlineComplex != nullptr)
        {
            validateComplex(element, *declaration.inlineComplex, path);
            return;
        }
        if (declaration.inlineSimple != nullptr)
        {
            if (!elementChildren(element).empty())
                addIssue(path, "Simple schema elements cannot contain elements.");
            validateSimpleRule(
                *declaration.inlineSimple,
                element.getAllSubText(),
                path);
            return;
        }
        if (declaration.typeName.isEmpty())
        {
            if (const auto referenced =
                    schema.elements.find(declaration.name);
                referenced != schema.elements.cend()
                && (!referenced->second.typeName.isEmpty()
                    || referenced->second.inlineComplex != nullptr
                    || referenced->second.inlineSimple != nullptr))
            {
                validateElement(element, referenced->second, path);
                return;
            }
        }
        if (element.hasAttribute("xsi:type"))
        {
            auto actualType =
                element.getStringAttribute("xsi:type");
            if (actualType.containsChar(':'))
                actualType =
                    actualType.fromLastOccurrenceOf(
                        ":",
                        false,
                        false);
            const auto actual = resolveType(actualType);
            std::set<juce::String> visiting;
            if (actual.complex == nullptr
                || declaration.typeName.isEmpty()
                || !derivesFrom(
                    actualType,
                    declaration.typeName,
                    visiting))
            {
                addIssue(
                    path + "/@xsi:type",
                    "The concrete type '" + actualType
                        + "' is not derived from '"
                        + declaration.typeName + "'.");
                return;
            }
            validateComplex(element, *actual.complex, path);
            return;
        }
        const auto type = resolveType(declaration.typeName);
        if (type.complex != nullptr)
        {
            validateComplex(element, *type.complex, path);
            return;
        }
        if (type.simple != nullptr
            || type.builtinSimple.isNotEmpty())
        {
            if (element.getNumAttributes() > 0)
                addIssue(path, "Simple schema elements cannot have attributes.");
            if (!elementChildren(element).empty())
                addIssue(path, "Simple schema elements cannot contain elements.");
            validateSimple(
                declaration.typeName,
                nullptr,
                element.getAllSubText(),
                path);
            return;
        }
        addIssue(
            path,
            "Schema type '" + declaration.typeName + "' could not be resolved.");
    }
};

DawProjectValidationResult validateDocument(
    const SchemaState& schemaState,
    const juce::String& rootName,
    const juce::String& xml)
{
    if (!schemaState.schema.has_value())
    {
        DawProjectValidationResult result;
        result.issues.push_back({ "/", schemaState.error });
        return result;
    }
    Validator validator(*schemaState.schema, rootName);
    auto result = validator.validate(xml);
    if (result.valid && rootName == "Project")
    {
        juce::XmlDocument document(xml);
        if (const auto root = document.getDocumentElement();
            root != nullptr
            && root->getStringAttribute("version") != "1.0")
        {
            result.valid = false;
            result.issues.push_back({
                "/Project/@version",
                "Studio Duo supports DAWproject version 1.0."
            });
        }
    }
    return result;
}

juce::String schemaHash(const SchemaState& state)
{
    if (state.data == nullptr || state.dataSize <= 0)
        return {};
    return juce::SHA256(
               state.data,
               static_cast<std::size_t>(state.dataSize))
        .toHexString()
        .toLowerCase();
}
}

juce::String DawProjectValidationResult::summary() const
{
    if (issues.empty())
        return "Schema validation passed.";
    juce::StringArray lines;
    for (const auto& issue : issues)
        lines.add(issue.objectPath + ": " + issue.message);
    return lines.joinIntoString("\n");
}

DawProjectValidationResult DawProjectSchemaValidator::validateProjectXml(
    const juce::String& xml)
{
    return validateDocument(projectSchemaState(), "Project", xml);
}

DawProjectValidationResult DawProjectSchemaValidator::validateMetadataXml(
    const juce::String& xml)
{
    return validateDocument(metadataSchemaState(), "MetaData", xml);
}

juce::String DawProjectSchemaValidator::projectSchemaSha256()
{
    return schemaHash(projectSchemaState());
}

juce::String DawProjectSchemaValidator::metadataSchemaSha256()
{
    return schemaHash(metadataSchemaState());
}
}
