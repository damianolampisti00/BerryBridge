#ifndef NETWORKMANAGERFACTORY_HPP_
#define NETWORKMANAGERFACTORY_HPP_

#include <QNetworkAccessManager>

#ifdef BERRYBRIDGE_HAVE_MBEDTLS
#include "mbedtlsnetworkaccessmanager.hpp"
typedef TlsNetworkAccessManager BerryNetworkAccessManager;
#else
typedef QNetworkAccessManager BerryNetworkAccessManager;
#endif

#endif /* NETWORKMANAGERFACTORY_HPP_ */
