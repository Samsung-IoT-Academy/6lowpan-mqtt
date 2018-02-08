Name:           6lowpan-mqtt
Version:        0.0.1
Release:        1
Summary:        Translator between 6LoWPAN application layer and MQTT broker
License:        GPL-2.0
Group:          Network & Connectivity/Other

Source0:        %{name}-%{version}.tar.gz
Source1001:     6lowpan-mqtt.service
Source1002:     mqtt.conf

BuildRequires:  mosquitto-devel
BuildRequires:  libopenssl-devel
BuildRequires:  libcares-devel

Requires:       libopenssl
Requires:       libcares
Requires:       libmosquitto1

%description
Translator between binary 6LoWPAN application layer and MQTT broker.
lora-mqtt is used to provide an interface between LoRa star network
and network layer and high-level protocols and applications


%prep
%setup -q

%build
make all
chmod +x bin/mqtt

%install
install -D -m 744 bin/mqtt  %{buildroot}%{_bindir}/6lowpan-mqtt
install -D -m 644 %{S:1001} %{buildroot}%{_unitdir}/%{name}.service
install -D -m 644 %{S:1002} %{buildroot}/etc/6lowpan-mqtt/mqtt.conf

%post
/bin/systemctl enable %{name}.service
/bin/systemctl start %{name}.service
/bin/systemctl daemon-reload

%files
%defattr(-,root,root,-)
%config(noreplace) %attr(-,root,root) /etc/6lowpan-mqtt/mqtt.conf
%{_bindir}/6lowpan-mqtt
%{_unitdir}/%{name}.service
