# Libocpp Code Generators
In this directory a collection of code generators for various purposes are located.

## C++ Code generator for v16 and v2
The script [generate_cpp.py](common/generate_cpp.py) can be used to generate datatypes, enums, and messages for v16 and v2.

```bash
python3 generate_cpp.py --schemas <json-schema-dir> --out <path-to-libocpp> --version <ocpp-version> 
```

e.g.

```bash
python3 generate_cpp.py --schemas ~/ocpp-schemas/v16/ --out ~/checkout/everest-workspace/libocpp --version v16 
```

```bash
python3 generate_cpp.py --schemas ~/ocpp-schemas/v2/ --out ~/checkout/everest-workspace/libocpp --version v2 
```

```bash
python3 generate_cpp.py --schemas ~/ocpp-schemas/v21/ --out ~/checkout/everest-workspace/libocpp --version v21 
```

## YAML code generator for EVerest types

The script [generate_everest_types.py](common/generate_cpp.py) can be used to generate EVerest YAML type definitions using OCPP2.0.1 and OCPP2.1 JSON schemas.

```bash
python3 generate_everest_types.py --schemas <json-schema-dir> --out <outfile> --types <comma-seperated-list-of-ocpp-types>
```

e.g.

```bash
python3 generate_everest_types.py --schemas ~/ocpp-schemas/v21/ --out ocpp.yaml --types ChargingNeedsType,ChargingScheduleType
```
