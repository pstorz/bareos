/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2013-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.
*/

#ifndef BAREOS_DIRD_SUBSCRIPTION_TRUST_ANCHOR_H_
#define BAREOS_DIRD_SUBSCRIPTION_TRUST_ANCHOR_H_

namespace directordaemon {

/*
 * Embedded test trust anchor for subscription-signing certificates.
 * Replace this PEM with the real Bareos subscription root certificate
 * when production signing material is available.
 */
inline constexpr const char kEmbeddedSubscriptionTrustAnchor[] = R"(-----BEGIN CERTIFICATE-----
MIIDfzCCAmegAwIBAgIUEs3NuHPujoM9DBfa+tUBjbEEPTwwDQYJKoZIhvcNAQEL
BQAwRzEmMCQGA1UEAwwdQmFyZW9zIFN1YnNjcmlwdGlvbiBUZXN0IFJvb3QxHTAb
BgNVBAoMFEJhcmVvcyBHbWJIICYgQ28uIEtHMB4XDTI2MDQxOTE3MDUwNloXDTM2
MDQxNjE3MDUwNlowRzEmMCQGA1UEAwwdQmFyZW9zIFN1YnNjcmlwdGlvbiBUZXN0
IFJvb3QxHTAbBgNVBAoMFEJhcmVvcyBHbWJIICYgQ28uIEtHMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAnktobW2j2S865GEZJIpBawl4QR/6eotdWhy1
qHiNQPmFxS/+vtFiyJFqxpjaJp2zq4/swl+6Zqww9K8HMru0mQKkiEDO2axTzcxN
whkZXX91badxKV5IbMoHpUo/4QEs8ZK9NXCF1+lJLsxSo69d0svnajrL98X9R2jm
Kc64tJgw/ewgzzpTvNjzoKIRpD1qvSx6DriS4ur2GA1zEhoYmIo20W7RXjCQM3xM
iaxgQfmXjkEz/asr4vW6v9hIyIRWf7i0rRVk1VePyvI8iOLayvNMhJPZw3m2eXlD
ZZm5OpH8syjpQlPT1EtUfWf/ij4ZCFy7kfdOt+D3Sy9iGU2MtQIDAQABo2MwYTAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBBjAdBgNVHQ4EFgQUN1QM20ph
W+jNNCOFIPgKETsUZj0wHwYDVR0jBBgwFoAUN1QM20phW+jNNCOFIPgKETsUZj0w
DQYJKoZIhvcNAQELBQADggEBAJpRuNbsbBObcscM6Ld2J7lGwf5VP0zlxueCzf4c
iJ+o7tmSh0acR9QyS6H1iGs6hRGCmiPhtYlWrfJvvT8NHn3hCC3TUabK0phC9Rm1
6oFvbpILhC6uswEqwA5E+8z2Po2ieyCL8Oc13gRHLwGsppRTA0tIJ5mHDywN3W58
Qq8NJDdi/rHZbkuI0RNmAcIEayGWKQnrW+p3Qs02lASAx+Ovl6dbbO2Dixz6Y7fc
Z7AlMin6RTcllPVNGiO5aTxUukJS0oq99ztJVpKCG1/sMP+28rRs7nZgZ9lI/T0h
s10S7a6voWxJ184i0Y30QOikEZLqz4FCBqZE8i3GyYIA90M=
-----END CERTIFICATE-----
)";

}  // namespace directordaemon

#endif  // BAREOS_DIRD_SUBSCRIPTION_TRUST_ANCHOR_H_
