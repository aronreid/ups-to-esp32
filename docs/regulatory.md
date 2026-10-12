# Regulatory: what selling this needs in Canada and the US

Written 2026-09-27 ahead of the Rev B run of 100 boards. This is the project's
reading of the rules, with sources. It is not legal advice, and a test lab
confirms what applies when it quotes.

**Nothing here is done except the module labels.** The board has not been
tested, so no compliance statement is engraved or printed anywhere yet.

## Two separate things are regulated

1. **The radio.** The ESP32-S3-WROOM-1 (Rev A: N4; Rev B: N16R8, same module
   family) is certified as a module: FCC ID **2AC7Z-ESPS3WROOM1**, ISED
   **21098-ESPS3WROOM1**. A host product inherits that certification if it
   leaves the module and its PCB antenna as certified, follows Espressif's
   integration rules (antenna keepout; see CLAUDE.md), and keeps people 20 cm
   or more from the antenna in normal use. Its grant requires the finished
   product to be labelled with the module's number where it can be seen.
2. **Everything else on the board.** The rest of the board is a "digital
   device" in its own right, and must be shown not to emit too much radio
   interference. The module's certification does not cover this.

## Canada (ISED)

| Requirement | Source | Status |
|---|---|---|
| "Contains IC: 21098-ESPS3WROOM1" on the product | RSS-Gen | **Done**: engraved under the tray (`tools/gen-case.py`, `REG_LINES`) |
| ICES-003 Issue 7, **Class B** (residential) emissions test | ICES-003, ICES-Gen | Not done |
| Label "CAN ICES-003(B)/NMB-003(B)" **on the device** | ICES-Gen | Not done: engrave only after passing |
| Licence-exempt radio notice in the manual, **English and French** | RSS-Gen | Not done |
| Test report kept for as long as the product is sold | ICES-Gen | Not done |

- **It's self-declared.** There's no ISED filing for ICES-003. The supplier in
  Canada (manufacturer or importer) is responsible for compliance and keeps
  the report. Boards built by JLCPCB and shipped to the owner make the owner
  the importer.
- **The label must be on the device.** ICES-Gen allows the label in the
  manual or on the packaging only for equipment 2.5 cm or smaller, or with
  ISED's prior written approval. This case is 41 × 69 mm. There's room under
  the tray for a third line beside the two module lines.
- **There is no hobby or small-quantity exemption.** ICES-Gen exempts only
  equipment used for research, development or demonstration, which must be
  marked "Demo unit". The rules attach to equipment "manufactured, imported,
  distributed, leased, offered for sale or sold". Boards used by the person
  who built them are almost certainly outside that. Boards given to others
  may count as distributed; this document does not settle it.
- **The manual notice** (RSS-Gen) begins: "This device contains
  licence-exempt transmitter(s)/receiver(s) that comply with Innovation,
  Science and Economic Development Canada's licence-exempt RSS(s). Operation
  is subject to the following two conditions: (1) This device may not cause
  interference. (2) This device must accept any interference, including
  interference that may cause undesired operation of the device." It is
  followed by the French text from RSS-Gen.

## United States (FCC)

| Requirement | Source | Status |
|---|---|---|
| "Contains FCC ID: 2AC7Z-ESPS3WROOM1" on the product | Module grant | **Done**: engraved under the tray |
| Part 15 Subpart B, **Class B** emissions test | 47 CFR 15.109 | Not done |
| Supplier's Declaration of Conformity (SDoC) | 47 CFR 2.906, 2.1077 | Not done |
| Compliance statement (15.19), Class B user information (15.105(b)), warning that changes void authority (15.21) | 47 CFR Part 15 | Not done: on the device or in the manual |

- **The responsible party for an SDoC must be located in the US.** A seller
  in Canada needs a US importer or distributor to take that role before
  selling into the US.
- **Home-built exemption.** 47 CFR 15.23 exempts up to five home-built
  devices that are not marketed. The Canadian rules have no equivalent.

## Selling from Canada into the US

Researched 2026-09-27. The tariff parts change month to month; check them with
a customs broker at the time of the first shipment.

### FCC: someone in the US has to be responsible

- **The SDoC's responsible party must be located in the US**
  ([47 CFR 2.909](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-2/subpart-J/subject-group-ECFR1d1c20dde929672/section-2.909)).
  For imported equipment that is the importer. A seller in Canada cannot fill
  the role. The options are:
  - a US distributor or fulfilment partner who imports the boards and takes
    the role;
  - a compliance firm offering a "US responsible party" service;
  - a US business of your own.
- **Each unit ships with a compliance information sheet** (47 CFR 2.1077):
  - what the product is (name and model);
  - a statement that it complies with Part 15;
  - the **US responsible party's name, US address, and a phone number or
    website**.

  It can be printed in the manual. Add it alongside the 15.19, 15.105(b) and
  15.21 text.
- **The same test report serves both countries.** Keep one copy for ISED and
  give one to the US responsible party.
- **FCC Form 740 (the old import declaration for radio devices) is gone** for
  a finished product like this: the FCC eliminated it in 2017 and kept it only
  for certain peripherals ([FCC: importation](https://www.fcc.gov/oet/ea/importation)).

### Customs: origin, not shipping point, sets the duty

- **The country of origin is probably China, not Canada.** JLCPCB builds the
  boards in China. US duty follows country of origin, not where a parcel
  ships from. Chinese-origin goods shipped from Canada do not get CUSMA/USMCA
  treatment. Flashing firmware and fitting a printed case in Canada may not
  change the origin: that depends on whether it counts as "substantial
  transformation", which needs a broker's view or a CBP ruling.
- **As of late August 2026** (per the [Zonos tariff tracker](https://zonos.com/docs/guides/us-tariff-changes),
  one aggregator, not CBP):
  - China-origin goods carry MFN duty plus a Section 301 tariff (12.5 % from
    2026-07-24), plus any older Section 301 list rates for the product's
    tariff code.
  - The IEEPA tariffs ended 2026-02-24. The Section 122 surcharge that
    replaced them expired 2026-07-24.
  - The 50 % Section 338 tariff on Canadian-origin goods (from 2026-08-22)
    covers listed categories; electronics are not among the examples given.
    If the origin were ever Canada, check whether this product's code is on
    the list.
- **There is no duty-free small-parcel allowance.** The $800 de minimis has
  been suspended since 2025-08-29, so every shipment pays duty, even one
  board.
- **The tariff code (HTS) sets the base rate** and which Section 301 lists
  apply. A Wi-Fi network adapter for a UPS could arguably fall under 8517.62
  (data communication apparatus) or 8543.70; a broker should classify it once.
- **Mark the country of origin in English** ("Made in China", or whatever the
  origin turns out to be) on the product or its packaging, as US customs
  requires for imports (19 USC 1304).
- **Send a commercial invoice with every shipment**, giving value, HTS code,
  country of origin and manufacturer. For direct-to-customer sales, decide
  who pays duty: DDP (you prepay, via a courier or a platform like Zonos) or
  DAP (the buyer pays on delivery, which surprises people).

### Canadian side of the export

- Most goods exported to the US need no Canadian export permit and no export
  declaration. The product uses Wi-Fi and TLS (mass-market encryption), which
  Canada's export controls treat leniently for the US. Not checked against the
  Export Control List line by line; confirm with the broker.
- If boards are imported into Canada and then re-exported, a **duty drawback**
  claim may recover the Canadian duty and GST paid on them.

### Checklist, US additions

1. Line up a US responsible party before the first US sale.
2. Add the 2.1077 compliance sheet (with the US party's contact details) to
   the manual.
3. Have a broker settle the HTS code and the country of origin, and quote the
   duty per board at current rates.
4. Put the origin marking on the product or its box.
5. Choose DDP or DAP for direct sales, and build duty into the price.

## One test covers both

The FCC Part 15B and ICES-003 Class B limits are essentially the same, and
labs routinely test both in one session. A pre-compliance scan first (a few
hundred to a couple of thousand dollars) shows whether a full test is worth
booking. For this board, the likely trouble spots are:

- **The USB cable to the UPS**, which radiates whatever noise rides on it.
- **The regulator's switching noise.** Rev B's BL9110 is linear, which helps.
- **The CH340C and the ESP32's clocks**, if the ground return is poor.

## Out of scope, or to check with the lab

- **Electrical safety.** CSA-type certification is aimed at mains-powered
  products. This board runs on 5 V USB from a separate adapter, so it's
  likely out of scope if the adapter is certified. Not confirmed against an
  ISED or provincial source.
- **Europe and the UK**, if sold there: CE marking under the Radio Equipment
  Directive, or UKCA, plus RoHS and WEEE. Not researched.

## Checklist before selling

1. Pre-compliance scan to FCC 15B and ICES-003 Class B; then the full test,
   or fixes and a re-scan.
2. Engrave "CAN ICES-003(B)/NMB-003(B)" under the tray (a third `REG_LINES`
   entry) once it passes.
3. Write the manual: the FCC 15.19, 15.105(b) and 15.21 text; the bilingual
   RSS-Gen notice; and the 20 cm RF-exposure condition from the module grant.
4. Keep the test report, and the SDoC for the US.
5. For US sales, see *Selling from Canada into the US* above.

## Sources

- FCC grant, module labelling: https://fccid.io/2AC7Z-ESPS3WROOM1
- ISED RSS-Gen: https://ised-isde.canada.ca/site/spectrum-management-telecommunications/en/devices-and-equipment/radio-equipment-standards/radio-standards-specifications-rss/rss-gen-general-requirements-compliance-radio-apparatus
- ISED ICES-Gen (labels, exemptions, test reports): https://ised-isde.canada.ca/site/spectrum-management-telecommunications/en/devices-and-equipment/interference-causing-equipment-standards-ices/ices-gen-general-requirements-compliance-interference-causing-equipment
- ISED ICES-003 Issue 7: https://ised-isde.canada.ca/site/spectrum-management-telecommunications/en/devices-and-equipment/interference-causing-equipment-standards-ices/ices-003-information-technology-equipment-including-digital-apparatus
- 47 CFR Part 15: https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15
- 47 CFR 2.909, responsible party: https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-2/subpart-J/subject-group-ECFR1d1c20dde929672/section-2.909
- FCC importation rules: https://www.fcc.gov/oet/ea/importation
- US tariff status (aggregator, check with a broker): https://zonos.com/docs/guides/us-tariff-changes
